"""
probe_prompt_derail.py

Controlled replay of the prompts that made Becky derail ("olec circulation..." and
"<CJK> indexing is..." as the very first tokens of an answer to a weather/temperature
question). Sends each prompt variant N times with the SAME system prompt and sampling
flags the app uses, and classifies how each answer STARTS. Generation is stopped early
(pause) once enough text is seen, so a full run takes minutes, not hours.

It also checks the one thing the app's stop logic depends on: that the unit still gives a
normal answer to the next request after a pause.

BEFORE RUNNING: leave the Offline Agent app on the CoreS3 (go back to the launcher), because
this script exits any llm task that exists on the unit. Restart the app afterwards.

    python3 probe_prompt_derail.py                # all variants, 10 trials each
    python3 probe_prompt_derail.py --trials 20    # more samples
    python3 probe_prompt_derail.py --only A,C,D   # just some variants
    python3 probe_prompt_derail.py --full         # let every answer run to the end (slow)
"""

import argparse
import json
import re
import socket
import time
import uuid

HOST = "127.0.0.1"
PORT = 10001

# Exactly what the app sends in llm.setup (copied from the unit's own journal).
SYSTEM_PROMPT = ("You are Becky, a friendly offline voice assistant. Avoid contractions in your "
                 "responses - write \"do not\" instead of \"don't\", \"I am\" instead of \"I'm\", "
                 "and so on.")
SETUP_DATA = {
    "model": "qwen2.5-1.5B-Int4-ax630c",
    "response_format": "llm.utf-8.stream",
    "input": "llm.utf-8",
    "enoutput": True,
    "max_token_len": 1023,
    "enable_temperature": True,
    "temperature": 0.6,
    "enable_repetition_penalty": True,
    "repetition_penalty": 1.1,
    "penalty_window": 50,
    "top_p": 0.9,
    "prompt": SYSTEM_PROMPT,
}

SENSORS = ("Current conditions: temperature is 31.4 degrees Celsius, humidity is 34 percent, "
           "pressure is 1008 hectopascals, air quality index is 58 out of 500 where lower is "
           "better, carbon dioxide level is about 537 parts per million.")

# A leading space on every prompt: ASR results arrive that way in the real app.
WEATHER_WORDS = ["degrees", "celsius", "31", "temperature", "weather", "humidity"]
NAME_NO_DATA_WORDS = ["weather", "augusto", "hello", "sorry", "do not", "access", "real-time",
                      "current", "temperature"]

VARIANTS = {
    "A": ("EXACT derailed prompt #1 (name + weather + sensors)",
          " My name is Augusto, can you tell me the weather now please?. " + SENSORS, WEATHER_WORDS),
    "B": ("EXACT derailed prompt #2 (name + temperature + sensors)",
          " My name is Augusto, can you tell me the temperature?. " + SENSORS, WEATHER_WORDS),
    "C": ("control: same as A WITHOUT the name",
          " Can you tell me the weather now please?. " + SENSORS, WEATHER_WORDS),
    "D": ("name + weather question, NO sensor sentence",
          " My name is Augusto, can you tell me the weather now please?.", NAME_NO_DATA_WORDS),
    "E": ("same as A but a different name (Maria)",
          " My name is Maria, can you tell me the weather now please?. " + SENSORS, WEATHER_WORDS),
    "F": ("sensor sentence BEFORE the name + question",
          " " + SENSORS + " My name is Augusto, can you tell me the weather now please?.", WEATHER_WORDS),
    "G": ("name + temperature, SHORT sensor sentence (temperature only)",
          " My name is Augusto, can you tell me the temperature?. Current conditions: temperature is "
          "31.3 degrees Celsius.", WEATHER_WORDS),
}

CJK = re.compile(r"[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af]")
HARD_TIMEOUT_S = 120.0
STOP_CHARS_DEFAULT = 160


def new_request_id():
    return str(uuid.uuid4())[:8]


class Client:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((HOST, PORT))
        self.buf = b""

    def send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())

    def read_one(self, timeout=5.0):
        self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        try:
            return json.loads(line.decode())
        except json.JSONDecodeError:
            return {"_raw": line.decode(errors="replace")}

    def drain(self, quiet_s=0.3):
        while self.read_one(timeout=quiet_s) is not None:
            pass


def cleanup(client, units):
    for unit in units:
        client.send({"request_id": new_request_id(), "work_id": unit, "action": "taskinfo"})
        resp = client.read_one(timeout=5.0)
        if not resp or resp.get("error", {}).get("code", -1) != 0:
            continue
        data = resp.get("data", [])
        for work_id in (data if isinstance(data, list) else []):
            client.send({"request_id": new_request_id(), "work_id": work_id, "action": "exit"})
            client.read_one(timeout=5.0)


def setup_and_wait(client, timeout=180.0):
    req_id = new_request_id()
    client.send({"request_id": req_id, "work_id": "llm", "action": "setup",
                 "object": "llm.setup", "data": SETUP_DATA})
    print(">> llm setup (same payload as the app) - the model takes ~20 s to load...")
    start = time.time()
    while time.time() - start < timeout:
        resp = client.read_one(timeout=timeout)
        if resp is None:
            break
        if resp.get("request_id") != req_id:
            continue
        if resp.get("error", {}).get("code", -1) != 0:
            raise RuntimeError(f"setup failed: {resp}")
        return resp.get("work_id")
    raise RuntimeError("setup timed out")


def run_trial(client, work_id, text, stop_chars):
    """Returns (answer_text, finished, was_paused, seconds_to_first_token)."""
    rid = new_request_id()
    client.send({"request_id": rid, "work_id": work_id, "action": "inference",
                 "object": "llm.utf-8", "data": text})
    out, finished, paused, t_pause, first_s = "", False, False, None, None
    t0 = time.time()
    while True:
        now = time.time()
        if now - t0 > HARD_TIMEOUT_S:
            break
        if paused and now - t_pause > 6.0:
            break
        msg = client.read_one(timeout=1.0)
        if msg is None:
            continue
        if msg.get("work_id") != work_id or msg.get("request_id") != rid:
            continue
        data = msg.get("data")
        if isinstance(data, dict):
            delta = data.get("delta", "")
            if delta and first_s is None:
                first_s = time.time() - t0
            out += delta
            if data.get("finish"):
                finished = True
                break
            if stop_chars and not paused and len(out) >= stop_chars:
                client.send({"request_id": new_request_id(), "work_id": work_id, "action": "pause"})
                paused, t_pause = True, time.time()
    client.drain(0.3)
    return out, finished, paused, first_s


def classify(answer, expect):
    head = answer[:200]
    if not answer.strip():
        return "EMPTY"
    if CJK.search(head):
        return "DERAIL (CJK in first 200 chars)"
    if "<|endoftext|>" in answer:
        return "DERAIL (end-of-text marker)"
    if expect and not any(k in head.lower() for k in expect):
        return "DERAIL (off-topic start)"
    return "ok"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=10)
    ap.add_argument("--only", type=str, default="")
    ap.add_argument("--full", action="store_true", help="do not stop answers early")
    args = ap.parse_args()
    keys = [k for k in VARIANTS if not args.only or k in args.only.upper().split(",")]
    stop_chars = 0 if args.full else STOP_CHARS_DEFAULT

    client = Client()
    print("Cleaning up any existing llm task (the app's, if it is running)...")
    cleanup(client, ["llm"])
    work_id = setup_and_wait(client)
    print(f"   llm task: {work_id}\n")

    results = {k: [] for k in keys}
    empty_after_pause = 0
    prev_paused = False

    # Interleave variants round-robin so a slow drift in the unit cannot favour one of them.
    for trial in range(1, args.trials + 1):
        for k in keys:
            label, prompt, expect = VARIANTS[k]
            answer, finished, paused, first_s = run_trial(client, work_id, prompt, stop_chars)
            verdict = classify(answer, expect)
            if prev_paused and not answer.strip():
                empty_after_pause += 1
            prev_paused = paused
            results[k].append((verdict, answer))
            shown = answer[:70].replace("\n", " ")
            print(f"[{trial:>2}/{args.trials}] {k}  {verdict:<31} first token {first_s if first_s is None else round(first_s, 1)}s  | {shown}")

    print("\n" + "=" * 96)
    print("SUMMARY  (a 'derail' = the answer's first 200 chars are CJK, off-topic, or contain <|endoftext|>)")
    print("=" * 96)
    for k in keys:
        label = VARIANTS[k][0]
        n = len(results[k])
        bad = sum(1 for v, _ in results[k] if v != "ok")
        print(f"{k}: {bad:>2}/{n} bad   {label}")
        for v, a in results[k]:
            if v == "EMPTY":
                print("      - EMPTY: no text came back at all")
            elif v != "ok":
                print(f"      - {v}: {a[:110]!r}")

    print("\nPAUSE CHECK (does the unit still answer after a pause?): ", end="")
    if args.full:
        print("not exercised in --full mode")
    elif empty_after_pause == 0:
        print("OK - every request after a pause produced an answer")
    else:
        print(f"WARNING - {empty_after_pause} request(s) right after a pause came back EMPTY; "
              "the app's stop logic would then leave the next turn without an answer")

    client.send({"request_id": new_request_id(), "work_id": work_id, "action": "exit"})
    client.read_one(timeout=5.0)
    print("\nDone. Restart the Offline Agent app on the CoreS3.")


if __name__ == "__main__":
    main()
