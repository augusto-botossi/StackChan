"""
probe_name_memory.py

Checks the name-memory attachment before it is flashed. The firmware (hal_offline_agent_callbacks.cpp)
keeps the user's name in RAM and appends "The user is <name>." to every question, IN FRONT of any
sensor sentence. This probe sends exactly that shape of prompt, with the firmware's real system prompt,
and measures:

  * derails           CJK text, <|endoftext|>, a leading ")" (the over-128-token failure)
  * readings used     the answer contains the number from the attached reading
  * name used         for "What is my name?" the answer contains the name
  * prompt size       the firmware's token estimate (ported below), after the same trimming that
                      fit_to_budget() in offline_agent_module.cc does, so the longest case (weather AND
                      position AND name) is tested as the unit would really receive it

    adb push probe_name_memory.py /opt/       (next to probe_prompt_derail.py)
    nohup python3 -u /opt/probe_name_memory.py > /opt/name_memory.log 2>&1 &
    python3 /opt/probe_name_memory.py --trials 6
    python3 /opt/probe_name_memory.py --wordings --trials 8     # compare ways of attaching the name
    python3 /opt/probe_name_memory.py --sensors --trials 8      # does the name in front hurt sensor answers?

A real log showed the firmware's current wording ("What is my name?. The user is Augusto.") makes
Becky greet instead of answer ("Hello, Augusto! How can I assist you today?"). --wordings sends the
name questions in several wordings and counts how many answers actually say the name.

Leave the Offline Agent app (CoreS3 launcher) before running; restart it afterwards.
"""

import argparse
import re

import probe_prompt_derail as P
from probe_sensor_prompts import CONTRACTION, REFUSAL, SYS_HINT

NAME = "Augusto"
NAME_SENTENCE = f" The user is {NAME}."
WEATHER = " Current conditions: temperature is 21.3 degrees Celsius, humidity is 45 percent, pressure is 1013 hectopascals."
POSITION = " Current position: latitude is 50.2575 degrees north, longitude is 8.6429 degrees east."
PROMPT_TOKEN_LIMIT = 120


def estimate_tokens(s):
    """Port of estimate_tokens() in offline_agent_module.cc (words + digits + punctuation, +15%)."""
    words = digits = punct = other = 0
    in_word = False
    for b in s.encode("utf-8"):
        if b >= 0x80:
            other += 1
            in_word = False
        elif chr(b).isalpha():
            if not in_word:
                words += 1
                in_word = True
        else:
            in_word = False
            if chr(b).isdigit():
                digits += 1
            elif chr(b) in "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~":
                punct += 1
    raw = words + digits + punct + other // 2
    return (raw * 115 + 99) // 100


def user_token_budget(system_prompt):
    fixed = estimate_tokens(system_prompt) + 5 + 8
    return PROMPT_TOKEN_LIMIT - fixed if PROMPT_TOKEN_LIMIT > fixed else 8


def fit_to_budget(base, extra, budget):
    """Port of fit_to_budget(): drop trailing sentences of the attachment until it fits."""
    while extra and estimate_tokens(base + extra) > budget:
        frm = len(extra) - 3 if len(extra) > 3 else 0
        cut = extra.rfind(". ", 0, frm + 2)
        extra = "" if cut == -1 else extra[:cut + 1]
    return base + extra


# key -> (question as ASR delivers it, attachment as the firmware builds it, proofs, what to count)
CASES = {
    "name+weather":  ("What is the weather like today?.", NAME_SENTENCE + WEATHER, ["21"], "reading"),
    "name+position": ("Where are we right now?.", NAME_SENTENCE + POSITION, ["50.25", "8.64"], "reading"),
    "name+both":     ("What is the weather like and where are we?.", NAME_SENTENCE + WEATHER + POSITION,
                      ["21", "50.25", "8.64"], "reading"),
    "ask-name":      ("What is my name?.", NAME_SENTENCE, [NAME.lower()], "name"),
    "greeting":      ("Hello Becky, how are you?.", NAME_SENTENCE, [NAME.lower()], "name"),
    "long-question": ("Can you please tell me what the temperature is outside and whether it is a good day "
                      "for a walk with the dog?.", NAME_SENTENCE + WEATHER, ["21"], "reading"),
}


# Ways of attaching the name to a question. The first one is what the firmware sends today; the others
# put the name FIRST (that needs a change in how the module builds the prompt, see fit_to_budget()).
WORDINGS = {
    "after: The user is X.":        lambda q: f" {q} The user is {NAME}.",
    "after: The user's name is X.": lambda q: f" {q} The user's name is {NAME}.",
    "before: The user is X.":       lambda q: f" The user is {NAME}. {q}",
    "before: The user's name is X": lambda q: f" The user's name is {NAME}. {q}",
    "before: My name is X.":        lambda q: f" My name is {NAME}. {q}",
}
NAME_QUESTIONS = ["What is my name?.", "Do you remember my name?.", "Can you tell me my name please?.",
                  "Hello Becky, how are you?."]


def classify(answer, proofs, kind):
    head = answer[:240]
    if not head.strip():
        return "empty"
    if P.CJK.search(head) or "<|endoftext|>" in head or head.lstrip().startswith(")"):
        return "derail"
    low = head.lower()
    if kind == "name":
        return "ok" if proofs[0] in low else "no-name"
    if any(p in head for p in proofs):
        return "ok"
    if REFUSAL.search(head):
        return "refused"
    return "other"


def compare_wordings(trials):
    client = P.Client()
    print("Cleaning up any existing llm task...")
    P.cleanup(client, ["llm"])
    P.SETUP_DATA = {**P.SETUP_DATA, "prompt": SYS_HINT}
    work_id = P.setup_and_wait(client)
    print(f"   llm task: {work_id}\n")
    budget = user_token_budget(SYS_HINT)
    results = {}   # (wording, question) -> [(verdict, answer)]
    for trial in range(1, trials + 1):
        for wname, build in WORDINGS.items():
            for q in NAME_QUESTIONS:
                prompt = build(q)
                if estimate_tokens(prompt) > budget:
                    print(f"  (skipped, {estimate_tokens(prompt)} > {budget} tokens: {prompt!r})")
                    continue
                answer, *_ = P.run_trial(client, work_id, prompt, 240)
                verdict = classify(answer, [NAME.lower()], "name")
                results.setdefault((wname, q), []).append((verdict, answer))
                print(f"[{trial:>2}/{trials}] {wname:<30} {q[:30]:<30} {verdict:<8} | {answer[:60]!r}")
    client.send({"request_id": P.new_request_id(), "work_id": work_id, "action": "exit"})
    client.read_one(timeout=5.0)

    print("\n" + "=" * 92)
    print('SUMMARY  (ok = the answer contains the name; for "How are you?" that means she used it, which is optional)')
    print("=" * 92)
    for wname in WORDINGS:
        print(f"\n{wname}")
        for q in NAME_QUESTIONS:
            lst = results.get((wname, q), [])
            vs = [v for v, _ in lst]
            if vs:
                print(f"    {q[:34]:<36} ok {vs.count('ok')}/{len(vs)}   derail {vs.count('derail')}")
        bad = [(q, a) for q in NAME_QUESTIONS for v, a in results.get((wname, q), []) if v != "ok"][:2]
        for q, a in bad:
            print(f"      e.g. {q[:24]!r}: {a[:90]!r}")
    print("\nDone. Restart the Offline Agent app on the CoreS3.")


# The name sentence FIRST, then the question, then the sensor sentence - what the firmware now sends -
# against no name at all and against the old wording (name after the question, before the reading).
SENSOR_VARIANTS = {
    "no name":                lambda q, extra: f" {q}{extra}",
    "name first (firmware)":  lambda q, extra: f" My name is {NAME}. {q}{extra}",
    "name after (old)":       lambda q, extra: f" {q} The user is {NAME}.{extra}",
}
SENSOR_QUESTIONS = {
    "weather":  ("What is the weather like today?.", WEATHER, ["21"]),
    "position": ("Where are we right now?.", POSITION, ["50.25", "8.64"]),
    "both":     ("What is the weather like and where are we?.", WEATHER + POSITION, ["21", "50.25", "8.64"]),
}


def compare_sensors(trials):
    client = P.Client()
    print("Cleaning up any existing llm task...")
    P.cleanup(client, ["llm"])
    P.SETUP_DATA = {**P.SETUP_DATA, "prompt": SYS_HINT}
    work_id = P.setup_and_wait(client)
    print(f"   llm task: {work_id}\n")
    budget = user_token_budget(SYS_HINT)
    results = {}
    for trial in range(1, trials + 1):
        for vname, build in SENSOR_VARIANTS.items():
            for qk, (q, extra, proofs) in SENSOR_QUESTIONS.items():
                # the firmware trims the attachment (from the end) when the prompt is too long: same here
                body_budget = budget - (estimate_tokens(f"My name is {NAME}. ") if "first" in vname else 0)
                fitted = fit_to_budget(q, extra, body_budget)
                prompt = build(q, fitted[len(q):])
                answer, *_ = P.run_trial(client, work_id, prompt, 240)
                verdict = classify(answer, proofs, "reading")
                results.setdefault((vname, qk), []).append((verdict, answer))
                print(f"[{trial:>2}/{trials}] {vname:<22} {qk:<9} {verdict:<8} | {answer[:70]!r}")
    client.send({"request_id": P.new_request_id(), "work_id": work_id, "action": "exit"})
    client.read_one(timeout=5.0)

    print("\n" + "=" * 92)
    print("SUMMARY  (ok = the answer contains the reading; for 'both', position was trimmed away by the budget)")
    print("=" * 92)
    for vname in SENSOR_VARIANTS:
        print(f"\n{vname}")
        for qk in SENSOR_QUESTIONS:
            vs = [v for v, _ in results.get((vname, qk), [])]
            contr = sum(1 for _, a in results.get((vname, qk), []) if CONTRACTION.search(a[:240]))
            print(f"    {qk:<10} ok {vs.count('ok')}/{len(vs)}   derail {vs.count('derail')}   "
                  f"refused {vs.count('refused')}   other {vs.count('other')}   contractions {contr}/{len(vs)}")
    print("\nDone. Restart the Offline Agent app on the CoreS3.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=6)
    ap.add_argument("--wordings", action="store_true", help="compare ways of attaching the name instead")
    ap.add_argument("--sensors", action="store_true", help="check sensor answers with the name in front")
    args = ap.parse_args()
    if args.wordings:
        return compare_wordings(args.trials)
    if args.sensors:
        return compare_sensors(args.trials)

    budget = user_token_budget(SYS_HINT)
    print(f"System prompt ~{estimate_tokens(SYS_HINT)} tokens; user budget {budget} (limit {PROMPT_TOKEN_LIMIT})\n")
    prompts = {}
    for key, (q, extra, proofs, kind) in CASES.items():
        full = estimate_tokens(q + extra)
        fitted = fit_to_budget(q, extra, budget)
        note = "" if fitted == q + extra else f"  -> TRIMMED to {estimate_tokens(fitted)}"
        print(f"  {key:<14} {full:>3} of {budget}{note}")
        prompts[key] = " " + fitted
    print()

    client = P.Client()
    print("Cleaning up any existing llm task...")
    P.cleanup(client, ["llm"])
    P.SETUP_DATA = {**P.SETUP_DATA, "prompt": SYS_HINT}
    work_id = P.setup_and_wait(client)
    print(f"   llm task: {work_id}\n")

    results = {}
    for trial in range(1, args.trials + 1):
        for key, (q, extra, proofs, kind) in CASES.items():
            answer, *_ = P.run_trial(client, work_id, prompts[key], 240)
            verdict = classify(answer, proofs, kind)
            results.setdefault(key, []).append((verdict, answer))
            print(f"[{trial:>2}/{args.trials}] {key:<14} {verdict:<8} | {answer[:70]!r}")
    client.send({"request_id": P.new_request_id(), "work_id": work_id, "action": "exit"})
    client.read_one(timeout=5.0)

    print("\n" + "=" * 92)
    print("SUMMARY  (ok = reading / name used; derail must be 0)")
    print("=" * 92)
    total_derail = 0
    for key, lst in results.items():
        vs = [v for v, _ in lst]
        contr = sum(1 for _, a in lst if CONTRACTION.search(a[:240]))
        total_derail += vs.count("derail")
        print(f"  {key:<14} ok {vs.count('ok')}/{len(vs)}   derail {vs.count('derail')}   "
              f"refused {vs.count('refused')}   no-name {vs.count('no-name')}   other {vs.count('other')}   "
              f"contractions {contr}/{len(vs)}")
        shown = 0
        for v, a in lst:
            if v != "ok" and shown < 2:
                print(f"      e.g. {v}: {a[:100]!r}")
                shown += 1
    print(f"\nTotal derails: {total_derail}")
    print("Done. Restart the Offline Agent app on the CoreS3.")


if __name__ == "__main__":
    main()
