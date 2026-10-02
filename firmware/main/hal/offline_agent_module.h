// offline_agent_module.h
//
// CoreS3-side driver for the offline (Module LLM native pipeline) voice
// assistant. Mirrors GpsModule's structure: owns its own UART, runs a
// background task, exposes a small public interface.
//
// This talks to Module LLM's StackFlow JSON protocol over UART (Port C,
// the same pins Module LLM already uses for power in the M5-Bus stack).
// Newline-delimited JSON, same {request_id, work_id, action, object, data}
// shape validated directly against the local socket over the past few
// days (see test_native_pipeline.py / test_native_llm.py results).
//
// Startup handles three things every single time, because Module LLM's
// own Linux boots independently of CoreS3 and may already have stale
// tasks running from a previous session:
//   1. Readiness probe (retry until Module LLM responds at all)
//   2. Cleanup of any existing kws/vad/asr/llm/melotts tasks
//   3. Fresh setup of the full chain
//
// AUDIO OUTPUT: melotts is configured with response_format "sys.pcm"
// and enoutput:false - the ACTUAL official pattern (confirmed from
// M5ModuleLLM Arduino library's own ApiMelottsSetupConfig_t default
// and M5Stack's Python "AI Pyramid" voice assistant reference), meaning
// Module LLM plays synthesized speech through its OWN onboard speaker
// directly and sends NOTHING back to us at all - zero audio data
// crosses the UART link in either direction for TTS.
//
// This followed two earlier, incorrect attempts, both stemming from
// leaving enoutput:true (correct for kws/vad/asr/llm, wrong for
// melotts): streaming raw PCM back over UART for CoreS3 playback
// ("pcm.stream.base64"), which hit persistent, unresolved data
// corruption on large messages even after three rounds of fixes; and a
// local-playback format that also echoed the full audio back
// ("sys.play.0_1" / "sys.play.0_1.stream"), which worked but saturated
// the UART link (fixed, non-configurable 115200 baud - confirmed
// llm-sys silently reverts any externally-forced baud change) with
// data we never used, causing multi-minute delays on longer responses.
//
// melotts is now chained directly to the LLM's own work_id at setup
// time ("tts.utf-8.stream" input chained to llm_work_id), matching
// M5Stack's own official Voice Assistant reference pattern, rather
// than us collecting the LLM's full response and sending one explicit
// inference call. Confirmed via direct testing to start speech ~51s
// earlier on a typical multi-sentence response. Since melotts still
// sends nothing back (enoutput:false), state transitions out of
// Speaking still rely on a text-length-based timing estimate (see
// handleLlmDelta() / update()) - though that estimate is currently
// still anchored to when the LLM finishes generating, not when speech
// actually starts - a known, deliberate placeholder pending a dedicated
// follow-up to redesign it properly for chained playback.
#pragma once
#include <driver/uart.h>
#include <string>
#include <vector>
#include <functional>
#include <mutex>

class OfflineAgentModule {
public:
    enum class State {
        Uninitialized,
        Idle,        // kws listening, nothing else active
        Listening,   // wake word detected, vad/asr active
        Processing,  // asr finished, waiting on llm
        Speaking,    // llm finished, melotts synthesizing/playing (on Module LLM's own speaker)
        FollowUp,    // speech just finished - vad/asr manually re-armed via "work" action,
                     // listening for a follow-up utterance WITHOUT requiring the wake word again
    };

    // rx_pin/tx_pin: Module LLM's UART pins on this device (Port C
    // default, unless re-pinned - see the GPS pin-conflict investigation
    // for why Port C should be free here).
    OfflineAgentModule(uart_port_t uart_num, int baud_rate, int tx_pin, int rx_pin);
    ~OfflineAgentModule();

    // Blocking. Runs the full startup sequence (readiness probe, cleanup,
    // setup chain). Returns false if Module LLM never becomes ready
    // within the timeout - caller should decide whether to retry or
    // surface an error to the user.
    bool start(uint32_t readiness_timeout_ms = 30000);

    // Non-blocking. Called from the app's main loop / update task to
    // drive the state machine forward (read available UART data, parse
    // JSON lines, react). Mirrors the polling style already used
    // elsewhere in this codebase (e.g. isXiaozhiStartRequested()).
    void update();

    State getState() const { return _state; }

    // Optional: let the caller (app layer) react to state transitions,
    // e.g. to drive avatar expressions the same way the cloud pipeline's
    // StateMachine does.
    using StateChangeCallback = std::function<void(State oldState, State newState)>;
    void onStateChange(StateChangeCallback cb) { _state_change_cb = cb; }

    // Optional: fired with the full response text right when melotts
    // inference is sent (i.e. right as Speaking begins) - lets the
    // caller show it as an on-screen caption, matching the cloud
    // pipeline's own SetChatMessage() behavior.
    using SpeechTextCallback = std::function<void(const std::string& text)>;
    void onSpeechText(SpeechTextCallback cb) { _speech_text_cb = cb; }

    // Optional: fired with the raw ASR text right before it's sent to
    // the LLM. Returns the text to actually send (unchanged, or with
    // sensor data appended). This is how GPS/environment sensor
    // readings get into the conversation - our local 1.5B LLM has no
    // structured tool-calling mechanism (unlike the cloud pipeline's
    // MCP tools, which the cloud LLM invokes itself mid-conversation),
    // so instead the caller (hal.cpp, which already has direct access
    // to GpsModule/EnvSensorModule) keyword-detects on the transcribed
    // text and injects a formatted reading only when relevant, rather
    // than every turn.
    using PromptAugmentCallback = std::function<std::string(const std::string& asr_text)>;
    void onBeforeLlmInference(PromptAugmentCallback cb) { _prompt_augment_cb = cb; }

private:
    struct Impl;
    Impl* _impl;

    State _state = State::Uninitialized;
    StateChangeCallback _state_change_cb;
    SpeechTextCallback _speech_text_cb;
    PromptAugmentCallback _prompt_augment_cb;

    void setState(State newState);

    // Startup sequence steps, each blocking with its own retry logic
    // (setup calls have empirically taken 20-90+ seconds on this
    // hardware - see the llm.setup investigation).
    bool waitForReady(uint32_t timeout_ms);
    void cleanupStaleTasks();
    bool setupKws();
    bool setupVad();
    bool setupAsr();
    bool setupLlm();
    bool setupMelotts();

    // Runtime message handling. Deliberately library-agnostic here (no
    // JSON type in this header) - the .cpp does the actual parsing
    // (ArduinoJson, per json_helper.cpp) and calls these with plain
    // extracted values.
    void handleLine(const std::string& jsonLine);
    void handleKwsWake(bool detected);
    void handleVadState(bool speechActive);
    void handleAsrResult(const std::string& text, bool finished);
    void handleLlmDelta(const std::string& textDelta, bool finished);
    // No audio data to handle anymore (Module LLM plays it itself) -
    // this just watches for the finish signal to know when speech is
    // actually done, so we can return to Idle and listen again.
    void handleMelottsFinished(bool finished);
    // Abandons a turn whose LLM stream went silent before sending finish:true
    // (see update()) so the device recovers instead of waiting forever.
    void handleLlmStall(uint32_t silent_ms);
    // Stops a generation that is repeating itself (see handleLlmDelta()).
    void handleLlmRunaway();
    // Stops the LLM when its end-of-text marker appears in the stream (see handleLlmDelta()).
    void handleLlmEndOfText(size_t marker_pos);

    void sendLlmInference(const std::string& text);

    // Sends a bare {"work_id":..., "action":"work"/"pause"} request -
    // confirmed via direct testing (test_pause_resume.py) to reliably
    // start/stop a unit's own mic subscription on an already-set-up
    // task, no re-setup needed. This is what drives the follow-up
    // window below: vad/asr are chained to kws at setup time and start
    // paused by design (see main_vad.cpp), only "waking" when kws
    // fires - sending "work" directly forces that same awake state
    // without a real wake word, and "pause" reverts them to their
    // normal dormant state.
    void sendWorkAction(const std::string& work_id);
    void sendPauseAction(const std::string& work_id);

    // link/unlink a unit's input to/from another unit's work_id at
    // runtime (matching README.md's documented "link" action) - used
    // specifically to temporarily disconnect melotts from the LLM's
    // output stream around a context reset, so it never hears (and
    // speaks) the reset's own "Context has been reset." confirmation.
    void sendLinkAction(const std::string& action, const std::string& work_id,
                        const std::string& target_work_id);

    // Follow-up window: after speech finishes, listen for a follow-up
    // utterance without requiring the wake word again, matching the
    // factory firmware's own default behavior (Application.cc:
    // kListeningModeAutoStop goes straight from Speaking to Listening,
    // not Idle). startFollowUpWindow() sends "work" to vad/asr and
    // records when it started; endFollowUpWindow() sends "pause" to
    // both, reverting to the normal kws-gated dormant state.
    void startFollowUpWindow();
    void endFollowUpWindow();
    uint32_t _followup_started_ms = 0;

    bool _finish_queued = false;  // guards against melotts's duplicate finish signal (confirmed via testing: it sends finished:true twice)

    // Text-length-based estimate for when melotts's local playback (via
    // Module LLM's own speaker) has likely finished. With
    // response_format "sys.pcm" + enoutput:false (see setupMelotts()),
    // melotts sends NOTHING back to us at all - by design, there is no
    // server-side completion signal to use even in principle, so this
    // estimate is the sole mechanism for detecting speech completion,
    // not a fallback for a real signal.
    //
    // Step 2 of the melotts-chaining redesign: since melotts now starts
    // speaking almost immediately and continues THROUGHOUT generation
    // (not after it), _turn_started_ms anchors to when the turn began
    // (prompt sent), not when generation finished - confirmed via real
    // production data that anchoring to generation-finish (the
    // previous design) added a large, mostly-redundant wait on top of
    // time speech had already been running: a 57.6s-to-generate
    // response added a FURTHER 71.4s wait afterward, even though speech
    // had almost certainly been running continuously for most/all of
    // that 57.6s already. _llm_finished_ms is the safeguard - we can
    // never transition before generation has actually finished (the
    // full text must exist before speech can possibly be complete), so
    // update() takes MAX(turn_started + estimate, llm_finished).
    uint32_t _turn_started_ms = 0;
    uint32_t _estimated_speech_ms = 0;
    uint32_t _llm_finished_ms = 0;

    // Proactive context-window management (see handleAsrResult() /
    // handleLlmDelta()): a running character-count estimate of
    // everything currently in the model's real 128-token context
    // (system prompt + every turn since the last reset), used to
    // trigger our OWN clean reset (with a brief continuity hint)
    // before the model's own automatic overflow-recovery would kick in
    // - confirmed via main_llm.cpp review to risk a stale-tokens_diff
    // mismatch against the freshly-reset cache, plausibly explaining a
    // real, observed bizarre/unrelated response once the ceiling was
    // exceeded. _reset_pending / _pending_prompt_after_reset hold state
    // across the brief unlink -> reset -> relink + send sequence.
    uint32_t _conversation_char_estimate = 0;
    bool _reset_pending = false;
    std::string _pending_prompt_after_reset;
    // Guards against deferring sensor/GPS data twice in a row (see
    // handleAsrResult()) - without this, a low reset threshold could
    // mean the real reading never actually gets through if the user
    // simply asks again.
    bool _sensor_data_just_deferred = false;

    // LLM stall watchdog (see update() / handleLlmStall()): time of the most
    // recent LLM output for the current turn, and whether that turn's
    // finish:true has been processed yet.
    uint32_t _last_llm_activity_ms = 0;
    bool _llm_turn_finished = true;

    // Runaway-loop guard: set once the current answer was cut off for repeating itself, so the
    // rest of that turn's stream is ignored; the next turn then starts with a clean context reset.
    bool _runaway_handled = false;
    bool _force_reset_next_turn = false;
    uint32_t _melotts_relink_due_ms = 0;  // 0 = nothing pending

    std::string _kws_work_id, _vad_work_id, _asr_work_id, _llm_work_id, _melotts_work_id;
    std::string _accumulated_asr_text;
    std::string _accumulated_llm_text;
};