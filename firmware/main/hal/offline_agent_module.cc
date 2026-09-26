// offline_agent_module.cc
#include "offline_agent_module.h"

#include <ArduinoJson.hpp>
#include <mooncake_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <system_info.h>
#include <cctype>
#include <algorithm>
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

// Workaround for an apparent bug in MeloTTS's own text normalization:
// contractions like "I'm" are spoken as "I delta am" - the literal
// field name we send the text under ("delta") appears to leak into
// speech during MeloTTS's own contraction-expansion step, which is
// inside M5Stack's closed binary and not something we can fix directly.
// Expanding common contractions ourselves before sending avoids
// triggering whatever's broken in its internal handling. Not
// exhaustive - covers the contractions most likely to appear in a
// casual assistant response; add more here if new ones are spotted.
static std::string expand_contractions(const std::string& text)
{
    static const std::vector<std::pair<std::string, std::string>> kContractions = {
        {"I'm", "I am"}, {"I've", "I have"}, {"I'll", "I will"}, {"I'd", "I would"},
        {"you're", "you are"}, {"you've", "you have"}, {"you'll", "you will"}, {"you'd", "you would"},
        {"we're", "we are"}, {"we've", "we have"}, {"we'll", "we will"}, {"we'd", "we would"},
        {"they're", "they are"}, {"they've", "they have"}, {"they'll", "they will"}, {"they'd", "they would"},
        {"he's", "he is"}, {"he'll", "he will"}, {"he'd", "he would"},
        {"she's", "she is"}, {"she'll", "she will"}, {"she'd", "she would"},
        {"it's", "it is"}, {"it'll", "it will"},
        {"that's", "that is"}, {"that'll", "that will"},
        {"what's", "what is"}, {"who's", "who is"}, {"here's", "here is"}, {"there's", "there is"},
        {"let's", "let us"},
        {"don't", "do not"}, {"doesn't", "does not"}, {"didn't", "did not"},
        {"can't", "cannot"}, {"couldn't", "could not"},
        {"won't", "will not"}, {"wouldn't", "would not"},
        {"isn't", "is not"}, {"aren't", "are not"}, {"wasn't", "was not"}, {"weren't", "were not"},
        {"shouldn't", "should not"}, {"haven't", "have not"}, {"hasn't", "has not"}, {"hadn't", "had not"},
    };

    std::string result = text;
    for (const auto& [contraction, expansion] : kContractions) {
        // Match the contraction with either a lowercase or capitalized
        // first letter (covers both mid-sentence and sentence-start
        // occurrences, e.g. "I'm" vs "i'm" never happens but "It's" vs
        // "it's" both do), leaving the rest of the word's case alone.
        for (const std::string& variant : {contraction, std::string(1, toupper(contraction[0])) + contraction.substr(1)}) {
            size_t pos = 0;
            while ((pos = result.find(variant, pos)) != std::string::npos) {
                std::string replacement = expansion;
                if (isupper(variant[0])) {
                    replacement[0] = toupper(replacement[0]);
                }
                result.replace(pos, variant.size(), replacement);
                pos += replacement.size();
            }
        }
    }
    return result;
}

void OfflineAgentModule::sendMelottsInference(const std::string& text)
{
    std::string expanded = expand_contractions(text);

    if (_speech_text_cb) {
        _speech_text_cb(expanded);
    }

    // ~13 characters/second, based on real measured data (not just a
    // guess): the 11.0 rate this replaced was found to systematically
    // OVERSHOOT for longer responses, not undershoot - confirmed
    // directly with a real 254-word/1491-character response, whose
    // natural speaking time at ~150 wpm (a standard average pace) would
    // be ~101.6s, but the 11.0 rate estimated 135.5s - roughly a third
    // too long. During that whole extra ~34 seconds, Becky had already
    // finished talking, but vad/whisper hadn't been triggered yet
    // (state stays "Speaking" until this estimate elapses), so nothing
    // was listening at all - a real, silent dead zone, not just
    // wasted time. 13 chars/sec derives directly from 150 wpm x ~5.1
    // average English characters per word / 60s. Still a heuristic,
    // not a measured value for MeloTTS specifically, and still the
    // sole mechanism for detecting speech completion (see
    // setupMelotts() - enoutput:false means there is no real signal to
    // fall back on) - further tuning may still be needed.
    constexpr double kCharsPerSecond = 13.0;
    // Digits take noticeably longer to speak than their character
    // count alone suggests - a single digit like "9" is spoken as a
    // whole word ("nine"), not a fraction of one, unlike typical
    // prose where a handful of characters is often part of one short
    // word. Confirmed directly: sensor-reading responses (containing
    // decimals like "24.6", "37.4", "999.0") caused the follow-up
    // window to open while Becky was still reading out the numbers,
    // and the TAIL of her own speech got picked up as a garbled
    // "hallucination" ("Zero head top that's your HAPE!", "999HP..")
    // - actually an echo of her own voice, not anything really said,
    // caused by the estimate undercounting specifically for
    // digit-heavy text. Starting value (0.25s/digit), not precisely
    // measured - may need further tuning.
    constexpr double kExtraSecondsPerDigit = 0.25;
    size_t digit_count = std::count_if(expanded.begin(), expanded.end(),
        [](unsigned char c) { return std::isdigit(c); });
    _speaking_started_ms = esp_timer_get_time() / 1000;
    _estimated_speech_ms = static_cast<uint32_t>(
        (expanded.size() / kCharsPerSecond) * 1000.0 + digit_count * kExtraSecondsPerDigit * 1000.0);

    std::lock_guard<std::mutex> lock(_impl->tx_mutex);
    // Sending "data" as a BARE STRING rather than {"delta": text} -
    // even a request with ONLY the "delta" key (no index/finish) still
    // produced "delta" spoken aloud, ruling out interference from
    // those other fields. This suggests melotts's inference handler
    // for this format doesn't expect an object at all, and some
    // fallback/stringification path is leaking the literal key name
    // "delta" into the synthesized text when it receives the wrong
    // shape of payload.
    ArduinoJson::JsonDocument doc;
    doc["request_id"] = new_request_id();
    doc["work_id"] = _melotts_work_id;
    doc["action"] = "inference";
    doc["object"] = "tts.utf-8";
    doc["data"] = expanded;  // bare string, not an object
    std::string body;
    ArduinoJson::serializeJson(doc, body);
    body += "\n";
    uart_write_bytes(_impl->uart_num, body.data(), body.size());
    mclog::tagInfo(_tag, "-> melotts inference: {}", expanded);
}

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
        std::string resp = read_line_blocking(_impl->uart_num, _impl->rx_buffer, 5000);
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
            read_line_blocking(_impl->uart_num, _impl->rx_buffer, 5000);  // discard the exit ack
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

    std::string resp = read_line_blocking(uart_num, rx_buffer, kSetupTimeoutMs);
    if (resp.empty()) {
        mclog::tagError(_tag, "{} setup: no response within {}ms", work_id, kSetupTimeoutMs);
        return false;
    }

    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, resp)) {
        mclog::tagError(_tag, "{} setup: failed to parse response", work_id);
        return false;
    }
    int error_code = doc["error"]["code"] | -1;
    if (error_code != 0) {
        mclog::tagError(_tag, "{} setup: error code {}", work_id, error_code);
        return false;
    }
    out_work_id = doc["work_id"].as<std::string>();
    mclog::tagInfo(_tag, "{} setup succeeded: {}", work_id, out_work_id);
    return true;
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
            data["prompt"] = "You are Becky, a friendly offline voice assistant.";
        },
        _llm_work_id);
}

bool OfflineAgentModule::setupMelotts()
{
    // response_format "sys.pcm" + enoutput:false - this is the ACTUAL
    // official pattern, confirmed from two independent sources:
    // M5ModuleLLM Arduino library's own ApiMelottsSetupConfig_t default
    // (enoutput = false), and M5Stack's own Python "AI Pyramid" voice
    // assistant reference implementation (same response_format,
    // explicit enoutput: false). We had enoutput:true this whole time -
    // copied from the pattern that's correct for kws/vad/asr/llm (where
    // we genuinely need their messages), but wrong for melotts. That
    // single flag was very likely the actual root cause of everything
    // fought over the past two days: melotts echoing its full audio
    // payload back to us on every chunk, saturating the UART link
    // (fixed at 115200 baud, confirmed non-configurable - llm-sys
    // silently reverts any externally-forced baud change) with data we
    // never used, causing both the transmission corruption we hit with
    // "pcm.stream.base64" and the multi-minute delays we measured with
    // "sys.play.0_1.stream".
    //
    // With enoutput:false, melotts sends NOTHING back to us at all -
    // Module LLM plays it locally, full stop. That also means there is
    // no server-side "finish" signal of any kind anymore (by design),
    // so state transitions rely entirely on the text-length timing
    // estimate (see sendMelottsInference / update()) - a fair trade
    // given the "real" signal was never practically usable for longer
    // responses anyway.
    return do_setup(_impl->uart_num, _impl->rx_buffer, _impl->tx_mutex,
        "melotts", "melotts.setup",
        [](ArduinoJson::JsonObject data) {
            data["model"] = "melotts-en-us";
            data["response_format"] = "sys.pcm";
            data["input"] = "tts.utf-8";
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
    // see the _speaking_started_ms / _estimated_speech_ms race below,
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

    // With enoutput:false (see setupMelotts()), melotts sends nothing
    // back to us at all - this timing estimate is the ONLY mechanism
    // for detecting speech completion, not a race against a real
    // signal anymore.
    //
    // Deliberately re-fetching the timestamp here rather than reusing
    // "now" from the top of this function: handleLine() above (via
    // handleLlmDelta -> sendMelottsInference) can set
    // _speaking_started_ms to a NEWER timestamp than "now" within this
    // SAME update() call, if the LLM's response finished on this exact
    // tick. Reusing the stale "now" then produced a NEGATIVE elapsed
    // time - which, in unsigned arithmetic, wraps around to a huge
    // value instead (confirmed directly: observed "elapsed" of
    // 4294967290ms, essentially UINT32_MAX), triggering an instant,
    // spurious "estimate elapsed" and cutting speech off immediately
    // after it started.
    if (_state == State::Speaking && !_finish_queued && _estimated_speech_ms > 0) {
        uint32_t fresh_now = esp_timer_get_time() / 1000;
        uint32_t elapsed = fresh_now - _speaking_started_ms;
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
        if (elapsed >= _estimated_speech_ms + kAcousticSettlingMs) {
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
    // No melotts fast-path anymore - with enoutput:false (see
    // setupMelotts()), melotts never sends us anything at all, so no
    // incoming line will ever match its work_id. State transitions out
    // of Speaking rely entirely on the timing estimate in update().

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
    }
    // melotts messages never reach here - the fast-path above always
    // intercepts them first, now that we've confirmed their actual
    // shape (a single giant non-streamed message, no delta/finish
    // fields at all).
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
    _accumulated_llm_text.clear();
    setState(State::Processing);
    // Logged/captioned text stays the original spoken words - only
    // what actually goes to the LLM gets augmented, so the on-screen
    // caption and logs still reflect what was really said.
    std::string augmented = _prompt_augment_cb ? _prompt_augment_cb(text) : text;
    sendLlmInference(augmented);
}

void OfflineAgentModule::handleLlmDelta(const std::string& textDelta, bool finished)
{
    if (_state != State::Processing) return;
    _accumulated_llm_text += textDelta;
    if (!finished) return;

    mclog::tagInfo(_tag, "LLM response: {}", _accumulated_llm_text);
    _finish_queued = false;  // reset before this turn's melotts finish signal arrives
    setState(State::Speaking);
    sendMelottsInference(_accumulated_llm_text);
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