// offline_agent_module.cc
#include "offline_agent_module.h"

#include <ArduinoJson.hpp>
#include <mooncake_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <system_info.h>
#include <utility>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* _tag = "OfflineAgent";

// StackFlow's own retry ceiling for its self-spawned tokenizer subprocess
// is 300 attempts - we saw llm.setup take 20-30+ seconds in practice,
// and once observed it exceed 45s (cold tokenizer start), hence 90s.
static constexpr uint32_t kSetupTimeoutMs = 90000;

// Total follow-up window budget - the actual, authoritative time we're
// willing to keep listening for a follow-up utterance, not just a
// rarely-triggered safety net. Needed because ASR's own "nothing heard"
// timeout fires much faster than useful for a real conversational
// pause - confirmed via testing: it returned an empty result within
// ~2 seconds of the window opening, far too fast for a person to
// notice a response ended and decide to reply. handleAsrResult() keeps
// re-arming (re-sending "work") on each empty result until this whole
// budget elapses, rather than giving up on the first one.
//
// Bumped 10000 -> 15000 -> 20000. The first bump addressed a genuine
// estimate-undercounting concern. This second bump is informed by
// direct evidence that ruled OUT that theory for the case that
// prompted it: VAD's own live state (see handleVadState()'s diagnostic
// logging) showed clean, accurate, continuous silence for the FULL
// window after a very long response - no sign Becky's speaker audio
// was leaking into the mic (which would show as spurious ACTIVE
// readings), and no sign of a mistimed window opening. The simpler
// remaining explanation: a long, information-dense response likely
// just needs more real human thinking/reaction time to formulate a
// follow-up than a short one does - not a timing/estimate bug.
static constexpr uint32_t kFollowUpWindowMs = 20000;

// Brief buffer between "estimated speech duration elapsed" and actually
// re-arming vad/whisper - see the usage site in update() for the full
// reasoning (confirmed acoustic echo/tail from Becky's own speaker).
// Starting value, not empirically tuned yet - may need adjustment based
// on further testing.
static constexpr uint32_t kAcousticSettlingMs = 800;

struct OfflineAgentModule::Impl {
    uart_port_t uart_num;
    std::string rx_buffer;  // accumulates partial lines between reads
    std::mutex tx_mutex;    // UART writes can come from update() and from
                             // the send*() helpers - keep them from
                             // interleaving, same lesson learned the hard
                             // way on the Tab5 firmware's send_frame().
};

// ---------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------

OfflineAgentModule::OfflineAgentModule(uart_port_t uart_num, int baud_rate, int tx_pin, int rx_pin)
{
    _impl = new Impl();
    _impl->uart_num = uart_num;

    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(uart_num, &uart_config);
    uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // Buffer sized generously - messages are now small (no audio data
    // crosses this link anymore, Module LLM plays its own TTS output),
    // but keeping this size costs nothing and leaves margin.
    uart_driver_install(uart_num, 8192, 8192, 0, nullptr, 0);

    mclog::tagInfo(_tag, "UART ready on port {}", (int)uart_num);

    // No audio codec dependency anymore - Module LLM plays its own TTS
    // output through its own onboard speaker (melotts configured with
    // response_format "sys.pcm" and enoutput:false). CoreS3 never
    // touches audio at all in this pipeline, in either direction.
}

OfflineAgentModule::~OfflineAgentModule()
{
    uart_driver_delete(_impl->uart_num);
    delete _impl;
}

void OfflineAgentModule::setState(State newState)
{
    if (newState == _state) return;
    State old = _state;
    _state = newState;
    if (_state_change_cb) {
        _state_change_cb(old, newState);
    }
}

// ---------------------------------------------------------------------
// Low-level send/receive helpers
// ---------------------------------------------------------------------

// Shared between setupLlm() (sent as the actual "prompt" field) and the
// context-window reset logic (its own length is the post-reset
// baseline for _conversation_char_estimate) - kept as one definition so
// they can never silently drift apart.
static const std::string kSystemPrompt =
    "You are Becky, a friendly offline voice assistant. "
    "Avoid contractions in your responses - write \"do not\" "
    "instead of \"don't\", \"I am\" instead of \"I'm\", and so on.";

static std::string new_request_id()
{
    // Simple enough for our purposes - StackFlow only seems to echo
    // this back for correlation, not enforce uniqueness across a
    // session. A counter would work just as well; using esp_random()
    // avoids adding a static counter to worry about thread-safety on.
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", (unsigned int)esp_random());
    return std::string(buf);
}

// Sends one JSON object, building it with ArduinoJson to stay
// consistent with the rest of this codebase (per json_helper.cpp).
static std::string build_request(const std::string& work_id, const std::string& action,
                                  const std::string& object,
                                  std::function<void(ArduinoJson::JsonObject)> fill_data = nullptr)
{
    ArduinoJson::JsonDocument doc;
    doc["request_id"] = new_request_id();
    doc["work_id"] = work_id;
    doc["action"] = action;
    if (!object.empty()) {
        doc["object"] = object;
    }
    if (fill_data) {
        auto data = doc["data"].to<ArduinoJson::JsonObject>();
        fill_data(data);
    }
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    out += "\n";
    return out;
}

void OfflineAgentModule::sendLlmInference(const std::string& text)
{
    std::lock_guard<std::mutex> lock(_impl->tx_mutex);
    std::string req = build_request(_llm_work_id, "inference", "llm.utf-8.stream",
        [&](ArduinoJson::JsonObject data) {
            data["delta"] = text;
            data["index"] = 0;
            data["finish"] = true;
        });
    uart_write_bytes(_impl->uart_num, req.data(), req.size());
    mclog::tagInfo(_tag, "-> llm inference: {}", text);
}

// NOTE: contractions like "I'm" being spoken as "I delta am" was a
// real, confirmed MeloTTS text-normalization bug - previously worked
// around here by expanding contractions in the text ourselves before
// sending it to melotts. That workaround is no longer possible: melotts
// is now chained directly to the LLM's own output (see setupMelotts()),
// so its text never passes through our code again. The system prompt
// in setupLlm() now asks the LLM itself to avoid contractions instead -
// less reliable than direct text rewriting, but the only lever chaining
// leaves us.

void OfflineAgentModule::sendWorkAction(const std::string& work_id)
{
    std::lock_guard<std::mutex> lock(_impl->tx_mutex);
    // Bare {"work_id":..., "action":"work"} - no "object" field, no
    // "data" field. Confirmed via direct testing (test_pause_resume.py)
    // to reliably force an already-set-up vad/asr task into its
    // "awake"/listening state, the same state kws normally triggers.
    std::string req = build_request(work_id, "work", "");
    uart_write_bytes(_impl->uart_num, req.data(), req.size());
    mclog::tagInfo(_tag, "-> {} work", work_id);
}

void OfflineAgentModule::sendPauseAction(const std::string& work_id)
{
    std::lock_guard<std::mutex> lock(_impl->tx_mutex);
    // Confirmed via testing: genuinely stops the task's mic
    // subscription entirely (not just a log statement) - see
    // main_asr.cpp / main_vad.cpp's task_pause().
    std::string req = build_request(work_id, "pause", "");
    uart_write_bytes(_impl->uart_num, req.data(), req.size());
    mclog::tagInfo(_tag, "-> {} pause", work_id);
}

void OfflineAgentModule::sendLinkAction(const std::string& action, const std::string& work_id,
                                        const std::string& target_work_id)
{
    // link/unlink's own "data" field is a bare string (the work_id to
    // connect to/from), not an object - build_request()'s fill_data
    // lambda always produces an object, so this constructs the request
    // directly instead, matching README.md's own documented example.
    std::lock_guard<std::mutex> lock(_impl->tx_mutex);
    ArduinoJson::JsonDocument doc;
    doc["request_id"] = new_request_id();
    doc["work_id"] = work_id;
    doc["action"] = action;
    doc["object"] = "work_id";
    doc["data"] = target_work_id;
    std::string req;
    ArduinoJson::serializeJson(doc, req);
    req += "\n";
    uart_write_bytes(_impl->uart_num, req.data(), req.size());
    mclog::tagInfo(_tag, "-> {} {} {} {}", work_id, action, "work_id", target_work_id);
}

void OfflineAgentModule::startFollowUpWindow()
{
    _followup_started_ms = esp_timer_get_time() / 1000;
    sendWorkAction(_vad_work_id);
    sendWorkAction(_asr_work_id);
    mclog::tagInfo(_tag, "Follow-up window started - listening without wake word");
}

void OfflineAgentModule::endFollowUpWindow()
{
    sendPauseAction(_vad_work_id);
    sendPauseAction(_asr_work_id);
}

// Blocking read of exactly one newline-delimited JSON line, or empty
// string on timeout. Mirrors the pattern already proven out in
// test_native_pipeline.py / test_native_llm.py, just in C++/UART
// instead of Python/TCP.
static std::string read_line_blocking(uart_port_t uart_num, std::string& rx_buffer, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    const uint32_t step_ms = 20;
    while (true) {
        auto nl = rx_buffer.find('\n');
        if (nl != std::string::npos) {
            std::string line = rx_buffer.substr(0, nl);
            rx_buffer.erase(0, nl + 1);
            return line;
        }
        if (waited >= timeout_ms) {
            return "";
        }
        uint8_t chunk[512];
        int len = uart_read_bytes(uart_num, chunk, sizeof(chunk), pdMS_TO_TICKS(step_ms));
        if (len > 0) {
            rx_buffer.append(reinterpret_cast<char*>(chunk), len);
        } else {
            waited += step_ms;
        }
    }
}

// ---------------------------------------------------------------------
// Startup sequence
// ---------------------------------------------------------------------

bool OfflineAgentModule::start(uint32_t readiness_timeout_ms)
{
    if (!waitForReady(readiness_timeout_ms)) {
        mclog::tagError(_tag, "Module LLM never became ready within {}ms", readiness_timeout_ms);
        return false;
    }

    cleanupStaleTasks();

    if (!setupKws())      { mclog::tagError(_tag, "kws.setup failed"); return false; }
    if (!setupVad())      { mclog::tagError(_tag, "vad.setup failed"); return false; }
    if (!setupAsr())      { mclog::tagError(_tag, "asr.setup failed"); return false; }
    if (!setupLlm())      { mclog::tagError(_tag, "llm.setup failed"); return false; }
    if (!setupMelotts())  { mclog::tagError(_tag, "melotts.setup failed"); return false; }

    _conversation_char_estimate = kSystemPrompt.size();
    setState(State::Idle);
    mclog::tagInfo(_tag, "Startup complete - idle, listening for wake word");
    return true;
}

bool OfflineAgentModule::waitForReady(uint32_t timeout_ms)
{
    // Module LLM boots its own independent Linux system - this can
    // take a genuinely long time relative to CoreS3's own near-instant
    // boot. Poll with a harmless taskinfo call rather than assuming any
    // fixed delay.
    uint32_t waited = 0;
    const uint32_t retry_interval_ms = 500;
    while (waited < timeout_ms) {
        {
            std::lock_guard<std::mutex> lock(_impl->tx_mutex);
            std::string req = build_request("kws", "taskinfo", "");
            uart_write_bytes(_impl->uart_num, req.data(), req.size());
        }
        std::string line = read_line_blocking(_impl->uart_num, _impl->rx_buffer, retry_interval_ms);
        if (!line.empty()) {
            mclog::tagInfo(_tag, "Module LLM responded - ready");
            return true;
        }
        waited += retry_interval_ms;
    }
    return false;
}

// Reads lines until one whose own "work_id" field exactly matches
// expected_work_id arrives, or the deadline passes - discarding
// anything else along the way. Confirmed necessary as a real fix, not
// just caution: without this, cleanupStaleTasks()'s own reads (below)
// were the likely SOURCE of stale messages that went on to poison
// later setup calls in do_setup() - e.g. a delayed exit acknowledgment
// read here, but actually consumed one step too late, left the REAL
// exit ack sitting in the buffer for some later, unrelated read to
// misinterpret as its own response.
static std::string read_matching_line(uart_port_t uart_num, std::string& rx_buffer,
                                       uint32_t timeout_ms, const std::string& expected_work_id)
{
    uint32_t start_ms = esp_timer_get_time() / 1000;
    while (true) {
        uint32_t elapsed_ms = (esp_timer_get_time() / 1000) - start_ms;
        if (elapsed_ms >= timeout_ms) return "";
        std::string resp = read_line_blocking(uart_num, rx_buffer, timeout_ms - elapsed_ms);
        if (resp.empty()) return "";

        ArduinoJson::JsonDocument doc;
        if (ArduinoJson::deserializeJson(doc, resp)) continue;
        std::string candidate_work_id = doc["work_id"] | "";
        if (candidate_work_id != expected_work_id) continue;
        return resp;
    }
}

void OfflineAgentModule::cleanupStaleTasks()
{
    // Learned the hard way during the native pipeline tests: previous
    // sessions (a crash, a power cycle mid-conversation) can leave
    // kws/vad/asr/llm/melotts tasks running, and re-running setup
    // without closing them first hits "task full" (error code -21).
    // "asr" kept alongside "whisper" for this one transition, in case
    // an old sense-voice task is still lingering from before the
    // switch - harmless no-op via taskinfo if nothing's there.
    const char* units[] = {"kws", "vad", "asr", "whisper", "llm", "melotts"};
    for (const char* unit : units) {
        std::string request_line;
        {
            std::lock_guard<std::mutex> lock(_impl->tx_mutex);
            request_line = build_request(unit, "taskinfo", "");
            uart_write_bytes(_impl->uart_num, request_line.data(), request_line.size());
        }
        // taskinfo's own response echoes the unit name (not a numbered
        // work_id) as its "work_id" field - e.g. querying "kws" gets
        // back a response with work_id "kws".
        std::string resp = read_matching_line(_impl->uart_num, _impl->rx_buffer, 5000, unit);
        if (resp.empty()) continue;

        ArduinoJson::JsonDocument doc;
        if (ArduinoJson::deserializeJson(doc, resp)) continue;
        if (!doc["data"].is<ArduinoJson::JsonArray>()) continue;

        for (auto work_id_val : doc["data"].as<ArduinoJson::JsonArray>()) {
            std::string work_id = work_id_val.as<std::string>();
            mclog::tagInfo(_tag, "Closing stale task: {}", work_id);
            std::lock_guard<std::mutex> lock(_impl->tx_mutex);
            std::string exit_req = build_request(work_id, "exit", "");
            uart_write_bytes(_impl->uart_num, exit_req.data(), exit_req.size());
            // exit's own response echoes back the exact work_id we sent
            // it for - validate rather than blindly discarding "the
            // next line", the same fix applied here as everywhere else.
            read_matching_line(_impl->uart_num, _impl->rx_buffer, 5000, work_id);
        }
    }
}

// Shared helper: send a setup request, wait (patiently) for a response,
// check its error code, and return the assigned work_id on success.
// Every setup call in this codebase has empirically taken anywhere
// from a couple seconds to 90+ seconds (see llm.setup investigation),
// so kSetupTimeoutMs is deliberately generous.
static bool do_setup(uart_port_t uart_num,
                     std::string& rx_buffer, std::mutex& tx_mutex,
                     const std::string& work_id, const std::string& object,
                     std::function<void(ArduinoJson::JsonObject)> fill_data,
                     std::string& out_work_id)
{
    std::string req;
    {
        std::lock_guard<std::mutex> lock(tx_mutex);
        ArduinoJson::JsonDocument doc;
        doc["request_id"] = "setup";
        doc["work_id"] = work_id;
        doc["action"] = "setup";
        doc["object"] = object;
        auto data = doc["data"].to<ArduinoJson::JsonObject>();
        fill_data(data);
        std::string body;
        ArduinoJson::serializeJson(doc, body);
        body += "\n";
        req = body;
        uart_write_bytes(uart_num, req.data(), req.size());
    }

    // Validate each response actually belongs to THIS request before
    // accepting it - confirmed directly as a real, serious bug without
    // this check: every setup call reuses the same literal request_id
    // ("setup"), so a stale message still sitting in the buffer (e.g. a
    // delayed exit acknowledgment from cleanupStaleTasks() moments
    // earlier) could get misread as an unrelated unit's setup response.
    // Observed concretely: kws's setup response contained "llm.1003",
    // llm's contained "kws.1004", melotts's contained "llm.1007" - the
    // wake word was then never recognized at all (chime played, but
    // _kws_work_id held the wrong value, so handleLine()'s matching
    // logic never fired), despite kws itself working correctly.
    // Loops past anything that doesn't start with the expected
    // "<unit>." prefix, respecting the overall timeout budget across
    // however many stale lines need skipping.
    std::string expected_prefix = work_id + ".";
    uint32_t start_ms = esp_timer_get_time() / 1000;
    while (true) {
        uint32_t elapsed_ms = (esp_timer_get_time() / 1000) - start_ms;
        if (elapsed_ms >= kSetupTimeoutMs) {
            mclog::tagError(_tag, "{} setup: no matching response within {}ms", work_id, kSetupTimeoutMs);
            return false;
        }
        std::string resp = read_line_blocking(uart_num, rx_buffer, kSetupTimeoutMs - elapsed_ms);
        if (resp.empty()) {
            mclog::tagError(_tag, "{} setup: no response within {}ms", work_id, kSetupTimeoutMs);
            return false;
        }

        ArduinoJson::JsonDocument doc;
        if (ArduinoJson::deserializeJson(doc, resp)) {
            mclog::tagError(_tag, "{} setup: failed to parse a response line, skipping", work_id);
            continue;
        }
        std::string candidate_work_id = doc["work_id"] | "";
        if (candidate_work_id.rfind(expected_prefix, 0) != 0) {
            mclog::tagInfo(_tag, "{} setup: ignoring stale/unrelated response (work_id={})", work_id, candidate_work_id);
            continue;
        }

        int error_code = doc["error"]["code"] | -1;
        if (error_code != 0) {
            mclog::tagError(_tag, "{} setup: error code {}", work_id, error_code);
            return false;
        }
        out_work_id = candidate_work_id;
        mclog::tagInfo(_tag, "{} setup succeeded: {}", work_id, out_work_id);
        return true;
    }
}

bool OfflineAgentModule::setupKws()
{
    return do_setup(_impl->uart_num, _impl->rx_buffer, _impl->tx_mutex,
        "kws", "kws.setup",
        [](ArduinoJson::JsonObject data) {
            data["model"] = "sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01";
            data["response_format"] = "kws.bool";
            data["input"] = "sys.pcm";
            data["enoutput"] = true;
            // Switched from "HELLO" to "HELLO BECKY" - a short, common
            // word like "HELLO" alone is genuinely prone to false
            // triggers from ambient conversation or audiobook/media
            // playback, since the sound simply occurs more often by
            // coincidence. A longer, more distinctive phrase (using the
            // robot's own name) is standard practice for reducing false
            // accepts - not yet empirically confirmed how well this
            // specific phrase performs with this KWS model, worth
            // watching over real use.
            data["kws"] = "HELLO BECKY";
        },
        _kws_work_id);
}

bool OfflineAgentModule::setupVad()
{
    return do_setup(_impl->uart_num, _impl->rx_buffer, _impl->tx_mutex,
        "vad", "vad.setup",
        [&](ArduinoJson::JsonObject data) {
            data["model"] = "silero-vad";
            data["response_format"] = "vad.bool";
            auto input = data["input"].to<ArduinoJson::JsonArray>();
            input.add("sys.pcm");
            input.add(_kws_work_id);
            data["enoutput"] = true;
        },
        _vad_work_id);
}

bool OfflineAgentModule::setupAsr()
{
    // Switched from sense-voice-small-10s (the "asr" unit) to
    // whisper-tiny (a SEPARATE unit, "whisper" - confirmed via
    // main_whisper.cpp: StackFlow("whisper"), not the same unit with a
    // swappable model despite the Arduino library sharing a struct name
    // for both).
    //
    // Switched tiny -> base after a controlled, side-by-side A/B test
    // (compare_asr_models.py) across sense-voice-small-10s and all
    // three whisper sizes on the same phrases: on simple phrases all
    // four performed equivalently, but on a proper-noun test case
    // ("Ted Lasso"), only whisper-base and whisper-small transcribed
    // it correctly - both sense-voice and whisper-tiny got it wrong
    // ("Ted Blaso" / "Ted Lazo"). Chose base over small since both
    // succeeded on this test but base is meaningfully smaller (259MB
    // vs 567MB) and faster per official benchmarks.
    //
    // response_format uses ".stream" so whisper emits the same
    // {"delta":..., "finish":..., "index":...} shape sense-voice used,
    // matching what handleLine()/handleAsrResult() already parse - the
    // documented default ("asr.utf-8", no stream) instead returns a
    // single plain string, which our existing parsing wouldn't handle.
    // Confirmed working correctly in real use with whisper-tiny; not
    // yet separately re-confirmed with base specifically, though the
    // mechanism (StackFlow's universal "stream" substring check) is
    // model-independent, so this should carry over directly.
    //
    // "language" is required for whisper (not used by sense-voice).
    return do_setup(_impl->uart_num, _impl->rx_buffer, _impl->tx_mutex,
        "whisper", "whisper.setup",
        [&](ArduinoJson::JsonObject data) {
            data["model"] = "whisper-base";
            data["response_format"] = "asr.utf-8.stream";
            auto input = data["input"].to<ArduinoJson::JsonArray>();
            input.add("sys.pcm");
            input.add(_kws_work_id);
            input.add(_vad_work_id);
            data["language"] = "en";
            data["enoutput"] = true;
        },
        _asr_work_id);
}

bool OfflineAgentModule::setupLlm()
{
    // Fields confirmed required by Riker's corrected payload - omitting
    // any of these was the original cause of the "config_body error"
    // we spent so long chasing before realizing it wasn't a crash.
    return do_setup(_impl->uart_num, _impl->rx_buffer, _impl->tx_mutex,
        "llm", "llm.setup",
        [](ArduinoJson::JsonObject data) {
            data["model"] = "qwen2.5-1.5B-Int4-ax630c";
            data["response_format"] = "llm.utf-8.stream";
            data["input"] = "llm.utf-8";
            data["enoutput"] = true;
            data["max_token_len"] = 1023;
            data["temperature"] = 0.7;
            data["top_p"] = 0.9;
            data["prompt"] = kSystemPrompt;
        },
        _llm_work_id);
}

bool OfflineAgentModule::setupMelotts()
{
    // Chained directly to the LLM's own work_id ("tts.utf-8.stream"
    // input chained to llm_work_id) - matching M5Stack's own official
    // Voice Assistant reference pattern, rather than us collecting the
    // LLM's full response and sending ONE explicit melotts inference
    // call afterward. Confirmed directly via probe_melotts_chaining.py:
    // on a typical multi-sentence response, speech started ~3.1s after
    // the prompt was sent, while the LLM didn't finish generating the
    // full text until ~54.7s in - a genuine ~51.6s of previously-silent
    // waiting time recovered. StackFlow feeds melotts tokens as the LLM
    // produces them internally, entirely within Module LLM - this
    // requires setupLlm() to have already run (see start()) so
    // _llm_work_id exists.
    //
    // Real trade-off worth being explicit about: since melotts now
    // receives the LLM's raw output directly (never passing through
    // our own code again), we can no longer preprocess the text before
    // synthesis - specifically, the contraction-expansion workaround
    // (fixing MeloTTS mispronouncing contractions) no longer applies at
    // all. Best available mitigation: ask the LLM itself, via the
    // system prompt, to avoid contractions (see setupLlm()) - less
    // reliable than direct text rewriting, but the only lever chaining
    // leaves us.
    //
    // response_format "sys.pcm" + enoutput:false unchanged - still the
    // correct official pattern, still zero audio data crossing the
    // UART link in either direction.
    //
    // KNOWN, TEMPORARY side effect of doing this in isolation (Step 1
    // of 2): our Speaking-state duration estimate (see
    // handleLlmDelta()) is still anchored to when the LLM FINISHES
    // generating, not when speech actually starts (~3s in per the
    // probe) - meaning follow-up-window timing will be measurably off
    // until Step 2 (redesigning the state-transition/estimate logic
    // for chained playback) is done as a separate, dedicated follow-up.
    return do_setup(_impl->uart_num, _impl->rx_buffer, _impl->tx_mutex,
        "melotts", "melotts.setup",
        [this](ArduinoJson::JsonObject data) {
            data["model"] = "melotts-en-us";
            data["response_format"] = "sys.pcm";
            auto input = data["input"].to<ArduinoJson::JsonArray>();
            input.add("tts.utf-8.stream");
            input.add(_llm_work_id);
            data["voice"] = "alloy";
            data["enoutput"] = false;
        },
        _melotts_work_id);
}

// ---------------------------------------------------------------------
// Runtime update loop
// ---------------------------------------------------------------------

void OfflineAgentModule::update()
{
    if (_state == State::Uninitialized) return;

    static uint32_t last_heartbeat = 0;
    uint32_t now = esp_timer_get_time() / 1000;
    bool heartbeat_fired = (now - last_heartbeat > 5000);
    if (heartbeat_fired) {
        last_heartbeat = now;
        mclog::tagInfo(_tag, "heartbeat - state={}, rx_buffer size={}", (int)_state, _impl->rx_buffer.size());
        SystemInfo::PrintHeapStats();
    }

    // Non-blocking: only consume what's already buffered/available.
    // Messages are all small now (no audio data crosses this link),
    // so there's no risk of this loop spending long stretches parsing
    // a single huge line the way melotts's audio chunks used to.
    uint8_t chunk[1024];
    int len = uart_read_bytes(_impl->uart_num, chunk, sizeof(chunk), 0);
    if (len > 0) {
        _impl->rx_buffer.append(reinterpret_cast<char*>(chunk), len);
    }

    // Process every complete line currently buffered WITHOUT erasing
    // after each one - erase() is O(remaining buffer size), so doing it
    // once per line is real, avoidable overhead. Kept as a reasonable
    // micro-optimization, but NOT the fix for the long-response delay
    // problem - confirmed by testing (identical ~120s result after
    // applying this alone). The actual cause is UART bandwidth: every
    // chunk still carries the full (unused) audio payload, and 115200
    // baud is a hard, non-configurable ceiling (see header comment) -
    // see the _turn_started_ms / _estimated_speech_ms logic below,
    // which is the real fix.
    size_t search_start = 0;
    size_t last_processed_end = 0;
    size_t nl;
    while ((nl = _impl->rx_buffer.find('\n', search_start)) != std::string::npos) {
        if (nl > last_processed_end) {
            std::string line = _impl->rx_buffer.substr(last_processed_end, nl - last_processed_end);
            if (!line.empty()) {
                handleLine(line);
            }
        }
        last_processed_end = nl + 1;
        search_start = last_processed_end;
    }
    if (last_processed_end > 0) {
        _impl->rx_buffer.erase(0, last_processed_end);
    }

    // FALLBACK mechanism now, not the sole one: the Option B native
    // helper (melotts_finish_helper.c, running separately on Module
    // LLM) polls the audio unit's real queue_status and injects a
    // genuine finish signal the moment playback is actually complete
    // (handled directly in handleLine()) - that fires first in
    // practice, and _finish_queued (checked in handleMelottsFinished())
    // prevents this estimate from firing again afterward. This estimate
    // still matters as a safety net for whenever the helper isn't
    // running (it's a separate process, not yet wired into boot).
    //
    // Deliberately re-fetching the timestamp here rather than reusing
    // "now" from the top of this function: handleLine() above (via
    // handleLlmDelta) can set _llm_finished_ms to a NEWER timestamp
    // than "now" within this SAME update() call, if generation finished
    // on this exact tick. Reusing the stale "now" would then produce a
    // NEGATIVE elapsed time - which, in unsigned arithmetic, wraps
    // around to a huge value instead (confirmed directly in an earlier
    // version of this logic: observed "elapsed" of 4294967290ms,
    // essentially UINT32_MAX).
    if (_state == State::Speaking && !_finish_queued && _estimated_speech_ms > 0) {
        uint32_t fresh_now = esp_timer_get_time() / 1000;
        // Step 2 of the melotts-chaining redesign: target time is
        // MAX(turn_started + estimate, llm_finished) - NOT simply
        // llm_finished + estimate (the original design, before
        // chaining). Since speech now starts almost immediately and
        // continues THROUGHOUT generation rather than after it, by the
        // time generation finishes speech may already be substantially
        // or entirely complete - confirmed directly via production
        // data: a 57.6s-to-generate response was previously followed by
        // a FURTHER 71.4s wait, even though speech had almost certainly
        // been running continuously for most/all of that 57.6s already.
        // The MAX still protects the other direction too - a response
        // that took unusually long to START generating won't get cut
        // off early, since we never transition before the full text
        // actually exists (_llm_finished_ms).
        uint32_t speaking_target = _turn_started_ms + _estimated_speech_ms;
        uint32_t target = (speaking_target > _llm_finished_ms) ? speaking_target : _llm_finished_ms;
        uint32_t elapsed = fresh_now - _turn_started_ms;
        // kAcousticSettlingMs: brief buffer before re-arming vad/whisper,
        // giving any echo/reverb from Becky's own just-finished speech
        // time to physically decay first. Confirmed real via direct
        // evidence, not speculation: ASR captured "You can't resist
        // you.." immediately after a response containing "You can't
        // resist me, can you?", and separately "That, I had too many
        // problems.." right after a response ending in "It had too
        // many problems." - both clear echoes of her own prior speech,
        // not anything actually said. An earlier test found no cross-
        // talk DURING active speech, but never tested this specific
        // acoustic-tail-end window right as playback ends.
        if (fresh_now >= target + kAcousticSettlingMs) {
            mclog::tagInfo(_tag, "Estimated speech duration elapsed ({}ms) - entering follow-up window", elapsed);
            handleMelottsFinished(true);
        }
    }

    // Follow-up window backstop - handleAsrResult() already handles the
    // normal retry-then-give-up path on each empty ASR result. This
    // covers the case where ASR/VAD stop producing ANY events at all
    // (not even empty ones) for some reason. Same stale-"now" pitfall
    // as above applies here too, since startFollowUpWindow() can be
    // called later in this same update() call (via
    // handleMelottsFinished(), reached through the check just above) -
    // re-fetch the timestamp rather than reuse "now".
    if (_state == State::FollowUp) {
        uint32_t fresh_now = esp_timer_get_time() / 1000;
        uint32_t elapsed = fresh_now - _followup_started_ms;
        if (elapsed >= kFollowUpWindowMs) {
            mclog::tagInfo(_tag, "Follow-up window timed out ({}ms) - no utterance detected, back to idle", elapsed);
            endFollowUpWindow();
            setState(State::Idle);
        }
    }
}

void OfflineAgentModule::handleLine(const std::string& jsonLine)
{
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, jsonLine)) {
        mclog::tagError(_tag, "Failed to parse line: {}", jsonLine);
        return;
    }

    std::string work_id = doc["work_id"] | "";
    if (work_id == _kws_work_id) {
        handleKwsWake(doc["data"] | false);
    } else if (work_id == _vad_work_id) {
        handleVadState(doc["data"] | false);
    } else if (work_id == _asr_work_id) {
        auto data = doc["data"];
        handleAsrResult(data["delta"] | "", data["finish"] | false);
    } else if (work_id == _llm_work_id) {
        auto data = doc["data"];
        handleLlmDelta(data["delta"] | "", data["finish"] | false);
    } else if (work_id == _melotts_work_id) {
        // Real signal from the Option B native helper (melotts_finish_helper.c,
        // running on Module LLM), polling the actual queue_status RPC action
        // and injecting this message the moment playback is genuinely
        // complete - not a timing estimate. melotts itself still sends
        // nothing (enoutput:false unchanged), so this only ever arrives
        // from the helper.
        //
        // Guarded to only act while genuinely in Speaking: the helper
        // fires this on ANY busy->idle transition in the audio unit's
        // queue, which includes the wake-word chime's own playback
        // finishing, not just real speech. Ignoring it outside Speaking
        // means a chime-triggered firing is simply harmless noise here,
        // rather than needing the helper itself to distinguish chime
        // playback from real speech.
        if (_state == State::Speaking && !_finish_queued) {
            auto data = doc["data"];
            if (data["finish"] | false) {
                mclog::tagInfo(_tag, "Real playback-complete signal received (Option B helper)");
                handleMelottsFinished(true);
            }
        }
    }
    // Anything else (echoes of our own setup/exit requests handled
    // synchronously elsewhere) is ignored here.
}

void OfflineAgentModule::handleKwsWake(bool detected)
{
    if (!detected) return;
    if (_state != State::Idle) return;  // ignore wake events mid-conversation
    mclog::tagInfo(_tag, "Wake word detected");
    _accumulated_asr_text.clear();
    setState(State::Listening);
}

void OfflineAgentModule::handleVadState(bool speechActive)
{
    // Temporary diagnostic: testing whether Module LLM's mic picks up
    // Becky's OWN speaker output as speech activity (a classic acoustic
    // self-hearing/echo issue) - if a long response's duration estimate
    // undercounts, she could still genuinely be talking when the
    // follow-up window opens, and her own voice leaking into the mic
    // could keep VAD continuously reporting "active" the whole window,
    // fully explaining why whisper produced zero results (not even an
    // empty one) during a recent 15-second follow-up window after a
    // very long (~370 character) response. Logged specifically during
    // Speaking/FollowUp, where this would actually matter.
    if (_state == State::Speaking || _state == State::FollowUp) {
        mclog::tagInfo(_tag, "VAD state (during {}): {}",
                        (_state == State::Speaking) ? "Speaking" : "FollowUp",
                        speechActive ? "ACTIVE" : "quiet");
    }
}

// Whisper is well known to hallucinate non-speech captions on silence
// or ambient noise - "[Music]", "[Applause]", "[Silence]" and similar,
// a documented characteristic of the model family (trained on large
// amounts of video/caption data), not something sense-voice-small-10s
// ever exhibited in our own testing. Confirmed directly: ASR produced
// "[Music]" during a follow-up window where VAD had just reported
// "quiet" and nothing was actually said or played.
//
// Also confirmed a second, equally common Whisper convention for the
// same kind of hallucination: parenthesized sound-effect descriptions
// like "(laughs)", "(applause)" - caught directly via "(cash
// register)" being sent through to the LLM as if it were real speech,
// since the original bracket-only check didn't catch it. Both
// conventions reliably wrap the ENTIRE result - real speech would
// never naturally transcribe that way - so checking both remains a
// safe, well-defined pattern without risking false positives on
// genuine short utterances.
static bool is_whisper_hallucination(const std::string& text)
{
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return false;
    size_t end = text.find_last_not_of(" \t.!?");
    if (end == std::string::npos || end < start) return false;
    bool bracketed = text[start] == '[' && text[end] == ']';
    bool parenthesized = text[start] == '(' && text[end] == ')';
    return bracketed || parenthesized;
}

void OfflineAgentModule::handleAsrResult(const std::string& text, bool finished)
{
    bool in_followup = (_state == State::FollowUp);
    if (_state != State::Listening && !in_followup) return;
    if (!finished) return;  // ASR resends the complete text on both the
                             // intermediate and final message - only
                             // act on the final one (seen directly in
                             // testing: two identical messages, only
                             // the second has finish=true).
    bool nothing_said = text.empty() || is_whisper_hallucination(text);
    if (nothing_said) {
        if (is_whisper_hallucination(text)) {
            mclog::tagInfo(_tag, "Ignoring likely Whisper hallucination: {}", text);
        }
        // Nothing was said (yet) - ASR's own "nothing heard" timeout is
        // much shorter than a real conversational pause (confirmed via
        // testing: ~2 seconds), so on its own this doesn't mean "give
        // up" - only that ASR itself stopped listening early. While
        // still within the overall follow-up budget, re-arm it and
        // keep waiting; only truly give up (pause + Idle) once that
        // whole budget has elapsed.
        if (in_followup) {
            uint32_t elapsed = (esp_timer_get_time() / 1000) - _followup_started_ms;
            if (elapsed < kFollowUpWindowMs) {
                sendWorkAction(_vad_work_id);
                sendWorkAction(_asr_work_id);
                return;  // stay in FollowUp, keep listening
            }
            endFollowUpWindow();
        }
        setState(State::Idle);
        return;
    }
    mclog::tagInfo(_tag, "ASR result: {}", text);
    setState(State::Processing);
    // Anchor point for the Step 2 duration-estimate redesign (see
    // update()) - the turn begins here, not when generation finishes.
    _turn_started_ms = esp_timer_get_time() / 1000;
    // Logged/captioned text stays the original spoken words - only
    // what actually goes to the LLM gets augmented, so the on-screen
    // caption and logs still reflect what was really said.
    std::string augmented = _prompt_augment_cb ? _prompt_augment_cb(text) : text;

    // Proactive context-window management: reset BEFORE we'd overflow
    // the model's real 128-token ceiling, rather than letting its own
    // automatic overflow-recovery handle it silently. Confirmed via
    // main_llm.cpp review: on overflow, SetKVCache() fails, the system
    // resets to just the system prompt, but the ALREADY-COMPUTED
    // tokens_diff (built against the old, longer context) gets applied
    // against the freshly-reset, shorter cache anyway - a real mismatch
    // that plausibly explains the bizarre, completely unrelated
    // math-problem response we saw once the ceiling was exceeded.
    // kResetThresholdChars (~100 tokens at a rough ~4 chars/token
    // estimate) leaves headroom below the real 128-token ceiling for
    // this turn's own question+answer to still fit.
    constexpr size_t kResetThresholdChars = 400;
    if (_conversation_char_estimate + augmented.size() >= kResetThresholdChars) {
        mclog::tagInfo(_tag, "Conversation approaching context limit ({} chars accumulated) - "
                             "resetting with a continuity hint", _conversation_char_estimate);
        // Brief, cheap continuity hint from the prior turn's own
        // response (truncated - injecting the WHOLE thing would defeat
        // the point of resetting at all), chosen over a silent reset
        // specifically for a voice assistant's sake: Becky should sound
        // like she's continuing the conversation, not restarting it.
        //
        // Skipped entirely if the prior response was abnormally long -
        // confirmed as a real, necessary guard via direct observation:
        // a response that ran the full ~261s to max_token_len (1023
        // tokens) turned out to be a rambling, unrelated, repetitive
        // hallucination (an off-topic CSS tutorial, in Chinese, with no
        // connection to the actual question asked). A healthy response
        // never comes close to that ceiling, so hitting it is itself a
        // warning sign. Without this guard, the hint would blindly
        // carry the hallucinated content forward into the next turn,
        // reinforcing it rather than giving the reset a genuine chance
        // to recover - confirmed directly: exactly this happened, with
        // Becky continuing to discuss CSS on the very next exchange.
        constexpr size_t kHintMaxChars = 80;
        constexpr size_t kRunawayResponseChars = 800;
        std::string prompt_prefix;
        if (_accumulated_llm_text.size() < kRunawayResponseChars) {
            std::string hint = _accumulated_llm_text.substr(0, kHintMaxChars);
            if (_accumulated_llm_text.size() > kHintMaxChars) hint += "...";
            prompt_prefix = "[Earlier we were discussing: " + hint + "] ";
        } else {
            mclog::tagInfo(_tag, "Prior response abnormally long ({} chars) - likely a runaway "
                                 "generation, skipping continuity hint", _accumulated_llm_text.size());
        }
        // Strip sensor/GPS bracket augmentation specifically for this
        // post-reset turn - confirmed as the real, consistent pattern
        // across every runaway failure seen so far: each one coincided
        // with a bracketed sensor/GPS reading landing as the very first
        // thing the model saw right after a reset, while the one reset
        // WITHOUT a bracket (a plain "How are you?") came back
        // completely normal. The live reading is deferred to whatever
        // natural follow-up comes next, once the context is already
        // warmed up with a real exchange - a less complete first
        // answer traded for avoiding a repeat of the catastrophic
        // runaway failures (270+ second responses of unrelated code,
        // random digits, or repetitive loops).
        std::string post_reset_text = augmented;
        if (augmented != text) {
            mclog::tagInfo(_tag, "Deferring sensor/GPS data past this reset - answering "
                                 "the plain question only this turn");
            post_reset_text = text;
        }
        _pending_prompt_after_reset = prompt_prefix + post_reset_text;
        _accumulated_llm_text.clear();
        _reset_pending = true;
        // melotts is chained directly to the LLM's own output stream
        // (see setupMelotts()) - it would otherwise hear and SPEAK the
        // reset's own "Context has been reset." reply, an awkward,
        // out-of-character moment right before the natural,
        // hint-continued answer. Unlinking first (re-linked once the
        // reset confirmation arrives - see handleLlmDelta()) prevents
        // it from ever being heard at all.
        sendLinkAction("unlink", _melotts_work_id, _llm_work_id);
        sendLlmInference("reset");
        return;
    }

    _accumulated_llm_text.clear();
    _conversation_char_estimate += augmented.size();
    sendLlmInference(augmented);
}

void OfflineAgentModule::handleLlmDelta(const std::string& textDelta, bool finished)
{
    if (_state != State::Processing) return;

    if (_reset_pending) {
        // Silently accumulate and discard the "Context has been
        // reset." confirmation - never spoken (melotts is unlinked -
        // see handleAsrResult()), never treated as a real answer. Once
        // it completes, re-link melotts, re-anchor timing for the REAL
        // turn about to start, and send the held, hint-augmented
        // prompt.
        if (!finished) return;
        _reset_pending = false;
        mclog::tagInfo(_tag, "Context reset confirmed - re-linking melotts and sending held prompt");
        sendLinkAction("link", _melotts_work_id, _llm_work_id);
        // Deliberate pause before sending the real prompt - testing
        // directly whether sending it too quickly races against the
        // model's own internal state (KV-cache/prefill) not yet being
        // fully settled by the time "Context has been reset." is sent
        // back to us. Confirmed as a real, consistent pattern via
        // direct observation across multiple fresh-boot tests: every
        // single runaway/garbled response seen so far (an unrelated
        // coding problem, a car-speed math problem, repeated hundreds
        // of digits) happened immediately after a reset, regardless of
        // the actual question's own content - strongly suggesting the
        // timing of the reset itself, not what's asked afterward, is
        // the real trigger.
        vTaskDelay(pdMS_TO_TICKS(300));
        _conversation_char_estimate = kSystemPrompt.size();
        _turn_started_ms = esp_timer_get_time() / 1000;
        std::string prompt = _pending_prompt_after_reset;
        _pending_prompt_after_reset.clear();
        sendLlmInference(prompt);
        return;
    }

    // Diagnostic: measure exactly when the LLM's FIRST token arrives,
    // relative to when this turn began (_turn_started_ms, set in
    // handleAsrResult() right before sending the prompt). Isolates how
    // much of the observed ~3s startup delay is the LLM's own
    // prefill/first-token cost specifically, versus ASR tail latency
    // (before the prompt was even sent) or melotts's own startup
    // (after this point) - not logged before, since we only ever
    // tracked the COMPLETE response's timing, never the first token.
    // Speaking state (mouth animation, LED) and the caption both now
    // start on the FIRST token, not the last - confirmed as a real,
    // noticeable UX gap otherwise: melotts is chained directly to the
    // LLM's own output (see setupMelotts()) and starts speaking almost
    // immediately, but the mouth/caption previously only appeared once
    // the ENTIRE response had finished generating (which, for a long
    // response, could be 30-90+ seconds AFTER audio had already been
    // playing) - Becky would be audibly talking with a static, closed
    // mouth and no caption for however long generation took.
    bool is_first_token = _accumulated_llm_text.empty() && !textDelta.empty();
    if (is_first_token) {
        uint32_t first_token_ms = (esp_timer_get_time() / 1000) - _turn_started_ms;
        mclog::tagInfo(_tag, "LLM first token received ({}ms after turn start)", first_token_ms);
        _finish_queued = false;  // reset before this turn's melotts finish signal arrives
        setState(State::Speaking);
    }

    _accumulated_llm_text += textDelta;

    // Caption updates progressively, live, on every delta - not just
    // once at the end with the complete text - so it visibly tracks
    // along with what's actually being spoken as it streams in.
    if (_speech_text_cb) {
        _speech_text_cb(_accumulated_llm_text);
    }

    if (!finished) return;

    mclog::tagInfo(_tag, "LLM response: {}", _accumulated_llm_text);
    _conversation_char_estimate += _accumulated_llm_text.size();

    // Now a plain watchdog, not an estimate: the Option B helper
    // (melotts_finish_helper, running natively on Module LLM) provides
    // a REAL completion signal as the primary mechanism (see
    // handleLine()'s handling of _melotts_work_id messages). This
    // timer's only remaining job is recovering from the rare case
    // where the helper isn't running at all - without it, Becky would
    // get stuck in Speaking forever, since nothing else would ever
    // trigger the transition.
    //
    // A flat, generous duration is simpler and more honest than a
    // tuned estimate now that accuracy isn't this timer's job -
    // confirmed directly that trying to keep it "accurate enough to
    // compete" caused real problems: with both mechanisms running,
    // whichever fired first won (via _finish_queued), and a tuned
    // estimate occasionally won that race and cut off a genuine,
    // still-in-progress sentence (real echo: "...released between 1977
    // to 2019." followed by ASR picking up "Chiki. They were released
    // between 1977 to 2019.." - her own tail end). 90s comfortably
    // exceeds the longest response duration seen so far (~88s).
    constexpr uint32_t kWatchdogTimeoutMs = 90000;
    _llm_finished_ms = esp_timer_get_time() / 1000;
    _estimated_speech_ms = kWatchdogTimeoutMs;
}

void OfflineAgentModule::handleMelottsFinished(bool finished)
{
    if (_state != State::Speaking) return;
    if (!finished) return;

    // Guard against a duplicate finish signal - melotts, like asr,
    // appears to send its final "finished:true" message twice
    // (confirmed in earlier testing: "Speech playback complete"
    // appearing twice, 2ms apart, before this guard was added).
    if (_finish_queued) return;
    _finish_queued = true;

    mclog::tagInfo(_tag, "Speech playback complete (on Module LLM's own speaker) - entering follow-up window");
    setState(State::FollowUp);
    startFollowUpWindow();
}