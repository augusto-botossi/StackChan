"""
probe_system_prompt.py

Why does Becky sometimes answer "I'm sorry, but I can't assist with that." to a plain request like
"Tell me a short story about a robot"? In the speech comparison it happened 1 time in 3 on EVERY
model (Int4, non-Int4, p256), so the suspect is the system prompt, not a model.

This probe tries several wordings of the system prompt on one model. For each wording it asks
  * creative requests (story, joke, poem)  -> counts refusals / apologies
  * questions with a sensor reading attached -> counts answers that actually use the reading
    (the last sentence of the current prompt was added for this: 18 of 18 used the reading with it)
and also counts contractions ("It's", "I'm") because the prompts say "Do not use contractions".
A wording is only better if it removes the refusals WITHOUT losing the readings.

No speech: it talks only to the llm unit (TCP port 10001), like probe_prompt_length.py. Answers are
stopped after about 120 characters, which is enough to see a refusal or a number.

BEFORE RUNNING: leave the Offline Agent app on the CoreS3 (go back to the launcher): the probe exits
the llm task that exists on the unit. Restart the app afterwards. Needs probe_prompt_derail.py in the
same folder.

    adb push probe_system_prompt.py /opt/
    python3 -u /opt/probe_system_prompt.py --model qwen2.5-1.5B-p256-ax630c > /opt/prompt_variants.log 2>&1
    python3 -u /opt/probe_system_prompt.py --trials 8 --only current,explicit     # fewer wordings, more samples

About 5 minutes per wording with the default 5 trials (each wording reloads the model, ~20 s).
"""

import argparse
import re
import time

import probe_prompt_derail as P

BASE = "You are Becky, a friendly voice assistant. Do not use contractions. "
THREE = "Answer in at most three short sentences unless asked for more. "
SENSORS = "You have sensors; use the readings given with a question."

# Same text as kSystemPrompt in offline_agent_module.cc for "current". Keep them in step.
VARIANTS = {
    "current":      BASE + THREE + SENSORS,
    "no_sensor":    BASE + THREE.rstrip(),
    "soft_sensor":  BASE + THREE + "When a reading is given with a question, use it.",
    "no_three":     BASE + SENSORS,
    "explicit":     BASE + THREE + "You are happy to tell stories, jokes and poems when asked. " + SENSORS,
}

# A leading space on every prompt, as ASR results arrive in the app. No name prefix: the firmware only
# adds it to questions about the name.
CREATIVE = [
    " Tell me a short story about a robot.",
    " Tell me a joke.",
    " Write a short poem about the sea.",
]
# (question with its reading attached, regex that the answer must match to count as "used the reading")
SENSOR = [
    (" Can you tell me the weather now please?. Current conditions: temperature is 31.4 degrees Celsius, "
     "humidity is 34 percent.", re.compile(r"31|34")),
    (" How about our current location?. Current position: latitude is 50.2575 degrees north, longitude is "
     "8.6429 degrees east.", re.compile(r"50\.?2|8\.?64|50|8\.6")),
    (" Is it too hot?. Current conditions: temperature is 28.7 degrees Celsius.", re.compile(r"28|29")),
]

REFUSAL = re.compile(r"(?i)^\s*(i'm|i am) sorry|\b(can't|cannot|can not|unable to|not able to)\b.{0,40}"
                     r"\b(assist|help|provide|do that|fulfill|write|tell|access)\b")
CONTRACTION = re.compile(r"(?i)\b\w+n't\b|\b(i'm|i've|i'll|i'd|you're|you've|you'll|you'd|we're|we've|they're|"
                         r"it's|that's|he's|she's|let's|there's|what's|here's)\b")
STOP_CHARS = 120


def run_variant(client, name, prompt, model, trials):
    P.cleanup(client, ["llm"])
    P.SETUP_DATA = {**P.SETUP_DATA, "model": model, "prompt": prompt}
    work_id = P.setup_and_wait(client)
    rows = []   # (kind, question, answer, first_s)
    for trial in range(1, trials + 1):
        for q in CREATIVE:
            ans, _fin, _pau, first = P.run_trial(client, work_id, q, STOP_CHARS)
            rows.append(("creative", q, ans, first))
        for q, rx in SENSOR:
            ans, _fin, _pau, first = P.run_trial(client, work_id, q, STOP_CHARS)
            rows.append(("sensor", q, ans, first, bool(rx.search(ans))))
        print(f"   [{name}] trial {trial}/{trials} done", flush=True)
    client.send({"request_id": P.new_request_id(), "work_id": work_id, "action": "exit"})
    client.read_one(timeout=5.0)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", type=str, default="qwen2.5-1.5B-p256-ax630c")
    ap.add_argument("--trials", type=int, default=5)
    ap.add_argument("--only", type=str, default="")
    args = ap.parse_args()
    names = [n for n in VARIANTS if not args.only or n in args.only.lower().split(",")]

    client = P.Client()
    print(f"Model under test: {args.model}; {args.trials} trials x {len(CREATIVE)} creative + "
          f"{len(SENSOR)} sensor questions per wording\n")
    results = {}
    for name in names:
        print(f">> wording '{name}': {VARIANTS[name]!r}", flush=True)
        results[name] = run_variant(client, name, VARIANTS[name], args.model, args.trials)

    print("\n" + "=" * 100)
    print(f"SUMMARY  model {args.model}   (refusal = an apology or 'I can't ...' in the first {STOP_CHARS} characters)")
    print("=" * 100)
    print(f"{'wording':<13} {'creative refused':>17} {'readings used':>14} {'contractions':>13} {'first token':>12}")
    for name in names:
        rows = results[name]
        cre = [r for r in rows if r[0] == "creative"]
        sen = [r for r in rows if r[0] == "sensor"]
        refused = sum(1 for r in cre if REFUSAL.search(r[2]))
        used = sum(1 for r in sen if r[4])
        contr = sum(1 for r in rows if CONTRACTION.search(r[2]))
        firsts = [r[3] for r in rows if r[3] is not None]
        avg_first = sum(firsts) / len(firsts) if firsts else float("nan")
        print(f"{name:<13} {refused:>9}/{len(cre):<7} {used:>8}/{len(sen):<5} {contr:>6}/{len(rows):<6} "
              f"{avg_first:>10.1f}s")

    print("\nREFUSED / READING NOT USED (first characters of each answer):")
    for name in names:
        for r in results[name]:
            if r[0] == "creative" and REFUSAL.search(r[2]):
                print(f"  [{name}] refused  {r[1].strip()[:40]!r:<44} -> {r[2][:80]!r}")
            if r[0] == "sensor" and not r[4]:
                print(f"  [{name}] no reading {r[1].strip()[:40]!r:<42} -> {r[2][:80]!r}")

    print("\nDone. Restart the Offline Agent app on the CoreS3.")


if __name__ == "__main__":
    main()
