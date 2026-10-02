"""
probe_sensor_prompts.py

Measures the prompts Becky will actually send after the length fix: the NEW short system prompt plus
the compact sensor/GPS sentence, for four question types, in three wordings:

  S1+after   what the firmware sends now: the question, then "Current conditions: ..."
  S1+before  the reading first, with a lead-in ("The robot's sensors report: ..."), then the question
  S2+after   like S1+after, but the system prompt also says she has sensors and should use readings

Each answer is classed as:
  ok       contains the number from the reading
  refused  no reading, and says sorry / can't / do not have / no access ...
  derail   CJK text, <|endoftext|>, or junk such as a leading ")"
  other    no reading and no refusal (made-up numbers, off-topic)
and also checked for contractions ("it's", "don't"), which the system prompt forbids.

Needs probe_prompt_derail.py in the same folder. Leave the Offline Agent app (CoreS3 launcher)
before running; restart it afterwards.

    adb push probe_sensor_prompts.py /opt/
    nohup python3 -u /opt/probe_sensor_prompts.py > /opt/sensor_prompts.log 2>&1 &
    python3 /opt/probe_sensor_prompts.py --trials 10        # more samples (about 20 minutes)
"""

import argparse
import re

import probe_prompt_derail as P

SYS_NEW = ("You are Becky, a friendly voice assistant. Do not use contractions. "
           "Answer in at most three short sentences unless asked for more.")
SYS_HINT = SYS_NEW + " You have sensors; use the readings given with a question."

# key -> (question as ASR delivers it, label, body of the reading, substrings that prove it was used)
QUESTIONS = {
    "weather": ("Can you tell me the weather now please?.", "Current conditions",
                "temperature is 31.4 degrees Celsius, humidity is 34 percent, pressure is 1008 hectopascals",
                ["31"]),
    "temperature": ("What is the temperature?.", "Current conditions",
                    "temperature is 31.4 degrees Celsius", ["31"]),
    "air": ("How is the air quality?.", "Current conditions",
            "air quality index is 58 out of 500 where lower is better, carbon dioxide level is about 537 parts per million",
            ["58", "537"]),
    "location": ("Can you tell me our current location please?.", "Current position",
                 "latitude is 50.2575 degrees north, longitude is 8.6429 degrees east", ["50.25", "8.64"]),
}


def after(q, label, body):
    return f" {q} {label}: {body}."


def before(q, label, body):
    return f" The robot's sensors report: {body}. {q}"


# (name, system prompt, prompt builder)
COMBOS = [
    ("S1+after ", SYS_NEW, after),
    ("S1+before", SYS_NEW, before),
    ("S2+after ", SYS_HINT, after),
]

REFUSAL = re.compile(r"sorry|can't|cannot|unable|do not have|don't have|no access|not able|real-time|"
                     r"as an ai|i am not", re.I)
CONTRACTION = re.compile(r"\b\w+'(?:s|t|re|ve|ll|d|m)\b", re.I)
STOP_CHARS = 240


def classify(answer, proofs):
    head = answer[:240]
    if not head.strip():
        return "empty"
    if P.CJK.search(head) or "<|endoftext|>" in head or head.lstrip().startswith(")"):
        return "derail"
    if any(p in head for p in proofs):
        return "ok"
    if REFUSAL.search(head):
        return "refused"
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=6)
    args = ap.parse_args()

    client = P.Client()
    results = {}   # (combo, question) -> list of (verdict, answer)
    for sys_prompt in dict.fromkeys(c[1] for c in COMBOS):          # one model load per system prompt
        combos = [c for c in COMBOS if c[1] == sys_prompt]
        print("Cleaning up any existing llm task...")
        P.cleanup(client, ["llm"])
        P.SETUP_DATA = {**P.SETUP_DATA, "prompt": sys_prompt}
        work_id = P.setup_and_wait(client)
        print(f"   llm task: {work_id}   system prompt: {sys_prompt!r}\n")
        for trial in range(1, args.trials + 1):
            for name, _, build in combos:
                for qk, (q, label, body, proofs) in QUESTIONS.items():
                    answer, *_ = P.run_trial(client, work_id, build(q, label, body), STOP_CHARS)
                    verdict = classify(answer, proofs)
                    results.setdefault((name, qk), []).append((verdict, answer))
                    print(f"[{trial:>2}/{args.trials}] {name} {qk:<11} {verdict:<8} | {answer[:70]!r}")
        client.send({"request_id": P.new_request_id(), "work_id": work_id, "action": "exit"})
        client.read_one(timeout=5.0)

    print("\n" + "=" * 92)
    print("SUMMARY  (ok = the answer contains the reading; refused / derail / other = it did not)")
    print("=" * 92)
    for name, _, _ in COMBOS:
        rows = [(qk, results[(name, qk)]) for qk in QUESTIONS if (name, qk) in results]
        allv = [v for _, lst in rows for v, _ in lst]
        n = len(allv)
        contr = sum(1 for _, lst in rows for _, a in lst if CONTRACTION.search(a[:240]))
        print(f"\n{name}  ok {allv.count('ok')}/{n}   refused {allv.count('refused')}   "
              f"derail {allv.count('derail')}   other {allv.count('other')}   "
              f"empty {allv.count('empty')}   answers with contractions {contr}/{n}")
        for qk, lst in rows:
            vs = [v for v, _ in lst]
            print(f"    {qk:<12} ok {vs.count('ok')}/{len(vs)}   refused {vs.count('refused')}   "
                  f"derail {vs.count('derail')}   other {vs.count('other')}")
        shown = 0
        for qk, lst in rows:
            for v, a in lst:
                if v not in ("ok",) and shown < 4:
                    print(f"      e.g. {qk} {v}: {a[:100]!r}")
                    shown += 1
    print("\nDone. Restart the Offline Agent app on the CoreS3.")


if __name__ == "__main__":
    main()
