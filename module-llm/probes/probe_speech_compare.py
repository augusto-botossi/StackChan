"""
probe_speech_compare.py

Runs Becky's REAL speaking path on the Module LLM, so you can LISTEN to a model and compare it with
another one: llm (current system prompt, same sampling flags as the app) chained directly into
melotts (sys.pcm, voice alloy, enoutput false) exactly like setupMelotts() in the firmware. The
audio comes out of the Module LLM's own speaker.

While it speaks, it logs the audio queue (audio_queue_log, a small helper) and measures what you
would notice as a listener:

  first token    seconds from sending the question until the LLM's first word
  first sound    seconds from sending the question until the speaker starts
  gaps           silences > 0.4 s BETWEEN sentences, while the answer is still being spoken
                 (the audible pauses; the queue is sampled about every 80 ms)
  speech         total seconds of audio

Every answer is printed in full so you can compare wording as well as timing.

BEFORE RUNNING: leave the Offline Agent app on the CoreS3 (go back to the launcher): this script
exits the llm and melotts tasks that exist on the unit. Restart the app afterwards. Run the two
models one after the other, never at the same time.

    adb push probe_speech_compare.py /opt/
    adb push audio_queue_log.c /opt/
    gcc /opt/audio_queue_log.c -lzmq -o /opt/audio_queue_log
    python3 -u /opt/probe_speech_compare.py > /opt/speech_int4.log 2>&1
    python3 -u /opt/probe_speech_compare.py --model qwen2.5-1.5B-p256-ax630c > /opt/speech_p256.log 2>&1

Needs probe_prompt_derail.py in the same folder (it reuses its connection code).
    --trials 3          repeats of every question (default 3)
    --only weather,name run just some questions
    --no-warmup         skip the first untimed "Hello." turn
"""

import argparse
import os
import re
import subprocess
import threading
import time

import probe_prompt_derail as P

# Same text as kSystemPrompt in offline_agent_module.cc. Keep the two in step.
SYSTEM_PROMPT = ("You are Becky, a friendly voice assistant. "
                 "Do not use contractions. "
                 "Answer in at most three short sentences unless asked for more. "
                 "You have sensors; use the readings given with a question.")

# A leading space on every prompt, as ASR results arrive in the app. The name prefix is what the
# firmware puts in front once it knows the name. All of them fit the 120-token budget.
QUESTIONS = {
    "weather": " My name is Augusto. Can you tell me the weather now please?. Current conditions: "
               "temperature is 31.4 degrees Celsius, humidity is 34 percent, pressure is 1008 "
               "hectopascals.",
    "name":    " My name is Augusto. What is my name?",
    "sky":     " My name is Augusto. Why is the sky blue?",
    "story":   " My name is Augusto. Tell me a short story about a robot.",
}

END_MARK = "<|endoftext|>"
CJK = re.compile(r"[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af]")
GAP_MIN_S = 0.4          # an idle stretch between two sentences counts as a pause above this
SETTLE_S = 3.5           # same settle time as the finish helper
HARD_TIMEOUT_S = 90.0
AUDIO_LOG_BIN = "/opt/audio_queue_log"


class AudioLog:
    """Reads 'ms busy|idle' lines from audio_queue_log; time base = time.monotonic()."""

    def __init__(self, path):
        self.events = []            # (t_seconds, busy)
        self.lock = threading.Lock()
        self.proc = subprocess.Popen([path], stdout=subprocess.PIPE, text=True, bufsize=1)
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            with self.lock:
                if self.events:
                    return
            time.sleep(0.05)
        raise RuntimeError("audio_queue_log produced no output (is the audio unit running?)")

    def _read(self):
        for line in self.proc.stdout:
            parts = line.split()
            if len(parts) == 2 and parts[0].isdigit():
                with self.lock:
                    self.events.append((int(parts[0]) / 1000.0, parts[1] == "busy"))

    def snapshot(self):
        with self.lock:
            return list(self.events)

    def idle_for(self):
        """Seconds the queue has been idle (0 when busy)."""
        ev = self.snapshot()
        if not ev or ev[-1][1]:
            return 0.0
        return time.monotonic() - ev[-1][0]

    def first_busy_after(self, t):
        for et, busy in self.snapshot():
            if busy and et >= t:
                return et
        return None

    def wait_idle(self, seconds, timeout=HARD_TIMEOUT_S):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.idle_for() >= seconds:
                return True
            time.sleep(0.1)
        return False

    def close(self):
        self.proc.terminate()


def setup_unit(client, unit, obj, data, timeout=180.0):
    rid = P.new_request_id()
    client.send({"request_id": rid, "work_id": unit, "action": "setup", "object": obj, "data": data})
    start = time.time()
    while time.time() - start < timeout:
        resp = client.read_one(timeout=timeout)
        if resp is None:
            break
        if resp.get("request_id") != rid:
            continue
        if resp.get("error", {}).get("code", -1) != 0:
            raise RuntimeError(f"{unit} setup failed: {resp}")
        return resp.get("work_id")
    raise RuntimeError(f"{unit} setup timed out")


def link_action(client, action, mel_id, llm_id):
    client.send({"request_id": P.new_request_id(), "work_id": mel_id, "action": action,
                 "object": "work_id", "data": llm_id})
    client.drain(0.3)


def analyse(events, t_send, t_text_done):
    """Busy intervals after t_send -> first sound, gaps between sentences, speech time, end time."""
    intervals, start = [], None
    state = False
    carry = None
    for t, busy in events:
        if t < t_send:
            state = busy
            continue
        if carry is None:
            carry = state
        if busy and start is None:
            start = t
        elif not busy and start is not None:
            intervals.append((start, t))
            start = None
    if start is not None:
        intervals.append((start, time.monotonic()))
    if not intervals:
        return {"carryover": bool(state if carry is None else carry)}
    gaps = []
    for (a0, a1), (b0, _b1) in zip(intervals, intervals[1:]):
        if b0 - a1 >= GAP_MIN_S:
            gaps.append(b0 - a1)
    return {
        "first_sound": intervals[0][0] - t_send,
        "audio_end": intervals[-1][1] - t_send,
        "speech": sum(b - a for a, b in intervals),
        "gaps": gaps,
        "carryover": bool(state if carry is None else carry),
    }


def run_turn(client, llm_id, mel_id, audio, text):
    audio.wait_idle(1.0)
    rid = P.new_request_id()
    t_send = time.monotonic()
    client.send({"request_id": rid, "work_id": llm_id, "action": "inference",
                 "object": "llm.utf-8", "data": text})
    out, first, stopped = "", None, ""
    while True:
        if time.monotonic() - t_send > HARD_TIMEOUT_S:
            stopped = "timeout"
            break
        msg = client.read_one(timeout=0.5)
        if msg is None:
            continue
        if msg.get("work_id") != llm_id or msg.get("request_id") != rid:
            continue
        data = msg.get("data")
        if not isinstance(data, dict):
            continue
        delta = data.get("delta", "")
        if delta and first is None:
            first = time.monotonic() - t_send
        out += delta
        if END_MARK in out:
            stopped = "end-of-text marker"
            break
        if len(out) >= 6 and CJK.search(out[:60]):
            stopped = "DERAILED (CJK text)"
            break
        if data.get("finish"):
            break
    t_text_done = time.monotonic()
    unlinked = False
    if stopped:
        # What the app does: stop the llm and unlink melotts so nothing after the marker is spoken.
        client.send({"request_id": P.new_request_id(), "work_id": llm_id, "action": "pause"})
        if mel_id:
            link_action(client, "unlink", mel_id, llm_id)
            unlinked = True
    out = out.split(END_MARK)[0]
    if mel_id and out.strip():
        # melotts needs several seconds before the first sound: wait for THIS answer's audio to
        # start, otherwise it would be counted against the next question.
        give_up = time.monotonic() + 30.0
        while time.monotonic() < give_up and audio.first_busy_after(t_send) is None:
            time.sleep(0.1)
    audio.wait_idle(SETTLE_S, timeout=HARD_TIMEOUT_S)
    if unlinked:
        link_action(client, "link", mel_id, llm_id)
    client.drain(0.3)
    res = analyse(audio.snapshot(), t_send, t_text_done) or {}
    res.update({"first_token": first, "text_done": t_text_done - t_send,
                "answer": out.strip(), "stopped": stopped})
    return res


def fmt(x, nd=1):
    return "-" if x is None else f"{x:.{nd}f}s"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", type=str, default=P.SETUP_DATA["model"])
    ap.add_argument("--trials", type=int, default=3)
    ap.add_argument("--only", type=str, default="")
    ap.add_argument("--no-warmup", action="store_true")
    ap.add_argument("--no-melotts", action="store_true",
                    help="llm only, no speech: tells whether the chained melotts causes a problem")
    ap.add_argument("--audio-log-bin", type=str, default=AUDIO_LOG_BIN)
    args = ap.parse_args()
    keys = [k for k in QUESTIONS if not args.only or k in args.only.lower().split(",")]

    if not os.path.exists(args.audio_log_bin):
        raise SystemExit(f"{args.audio_log_bin} not found: push audio_queue_log.c to /opt and compile "
                         "it (gcc /opt/audio_queue_log.c -lzmq -o /opt/audio_queue_log)")

    print(f"Model under test: {args.model}   (listen to the speaker; answers are printed in full)")
    client = P.Client()
    print("Cleaning up any existing llm / melotts task (the app's, if it is running)...")
    P.cleanup(client, ["llm", "melotts"])
    audio = AudioLog(args.audio_log_bin)

    llm_data = {**P.SETUP_DATA, "model": args.model, "prompt": SYSTEM_PROMPT}
    print(">> llm setup (the model takes ~20 s to load)...")
    llm_id = setup_unit(client, "llm", "llm.setup", llm_data)
    print(f"   llm task: {llm_id}")
    mel_id = None
    if args.no_melotts:
        print(">> --no-melotts: llm only, nothing will be spoken\n")
    else:
        print(">> melotts setup, chained to the llm (same as the app)...")
        mel_id = setup_unit(client, "melotts", "melotts.setup", {
            "model": "melotts-en-us", "response_format": "sys.pcm",
            "input": ["tts.utf-8.stream", llm_id], "voice": "alloy", "enoutput": False})
        print(f"   melotts task: {mel_id}\n")

    if not args.no_warmup:
        print(">> warm-up turn (not counted)...")
        w = run_turn(client, llm_id, mel_id, audio, " Hello.")
        print(f"   warm-up answer: {w['answer'][:150]}" + (f"  [stopped: {w['stopped']}]" if w["stopped"] else ""))

    results = {k: [] for k in keys}
    for trial in range(1, args.trials + 1):
        for k in keys:
            r = run_turn(client, llm_id, mel_id, audio, QUESTIONS[k])
            results[k].append(r)
            gaps = r.get("gaps", [])
            note = f"  [stopped: {r['stopped']}]" if r["stopped"] else ""
            if r.get("carryover"):
                note += "  [WARNING: audio of the previous answer was still playing, timing unreliable]"
            print(f"[{trial}/{args.trials}] {k:<8} first token {fmt(r['first_token'])}  "
                  f"first sound {fmt(r.get('first_sound'))}  text done {fmt(r['text_done'])}  "
                  f"speech {fmt(r.get('speech'))}  pauses {len(gaps)}"
                  f"{' (longest ' + fmt(max(gaps)) + ')' if gaps else ''}{note}")
            print(f"        answer: {r['answer'][:400]}")

    print("\n" + "=" * 98)
    print(f"SUMMARY  model {args.model}  (pauses = silences > {GAP_MIN_S}s between sentences)")
    print("=" * 98)
    print(f"{'question':<9} {'first token':>11} {'first sound':>11} {'speech':>8} {'pauses/turn':>12} "
          f"{'longest pause':>14}")

    def avg(vals):
        vals = [v for v in vals if v is not None]
        return sum(vals) / len(vals) if vals else None

    for k in keys:
        rs = results[k]
        allgaps = [g for r in rs for g in r.get("gaps", [])]
        print(f"{k:<9} {fmt(avg([r['first_token'] for r in rs])):>11} "
              f"{fmt(avg([r.get('first_sound') for r in rs])):>11} "
              f"{fmt(avg([r.get('speech') for r in rs])):>8} "
              f"{len(allgaps) / len(rs):>12.1f} {fmt(max(allgaps) if allgaps else None):>14}")

    if mel_id:
        client.send({"request_id": P.new_request_id(), "work_id": mel_id, "action": "exit"})
        client.read_one(timeout=5.0)
    client.send({"request_id": P.new_request_id(), "work_id": llm_id, "action": "exit"})
    client.read_one(timeout=5.0)
    audio.close()
    print("\nDone. Restart the Offline Agent app on the CoreS3.")


if __name__ == "__main__":
    main()
