"""
probe_prompt_length.py

Proves (or disproves) that PROMPT LENGTH alone makes the model derail.

In the replay (probe_prompt_derail.py) every prompt of 131-133 tokens derailed (40 of 40) and a
127-token control never did (10 of 10) - but those prompts also differed in content. This test
holds the content constant: the same weather question, padded with the word " please" to EXACT
total token counts on either side of the model's 128-token prefill window. Token counts are
measured with the unit's own tokenizer and chat template, so they are exact.

Needs probe_prompt_derail.py in the same folder (it reuses its connection code and settings).
Leave the Offline Agent app (go back to the CoreS3 launcher) before running; restart it afterwards.

    adb push probe_prompt_length.py /opt/
    python3 /opt/probe_prompt_length.py                 # 12 lengths x 6 trials, ~10 minutes
    python3 /opt/probe_prompt_length.py --trials 10
    python3 /opt/probe_prompt_length.py --lengths 126,127,128,129,130

To check another model, e.g. the 256-token one (install it first, then look in /opt/m5stack/data/
for its folder name - the tokenizer folder inside it is what --tokenizer-dir needs):

    python3 /opt/probe_prompt_length.py --model qwen2.5-1.5B-p256-ax630c \\
        --tokenizer-dir /opt/m5stack/data/qwen2.5-1.5B-p256-ax630c/tokenizer \\
        --lengths 240,250,254,255,256,257,258,270,300
"""

import argparse

import probe_prompt_derail as P

TOKENIZER_DIR = "/opt/m5stack/data/qwen2.5-1.5B-Int4-ax630c/tokenizer"
BASE = (" Can you tell me the weather now please?. Current conditions: temperature is 31.4 degrees "
        "Celsius, humidity is 34 percent.")
DEFAULT_LENGTHS = [100, 110, 120, 124, 126, 127, 128, 129, 130, 132, 136, 144]
PAD = " please"


def load_tokenizer(tokenizer_dir):
    try:
        from transformers import AutoTokenizer
    except ImportError as e:
        raise SystemExit(f"transformers is not importable here ({e}); it is what the unit's own "
                         "tokenizer script uses, so run this with the same python3 as that script")
    return AutoTokenizer.from_pretrained(tokenizer_dir)


def count_tokens(tok, user_text):
    """Exact length of the prompt the unit sends to the model (same template as its tokenizer script)."""
    messages = [{"role": "system", "content": P.SETUP_DATA["prompt"]},
                {"role": "user", "content": user_text}]
    text = tok.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
    return len(tok.encode(text))


def build_prompt(tok, target):
    user = BASE
    n = count_tokens(tok, user)
    if n > target:
        raise SystemExit(f"the base prompt is already {n} tokens, above the target {target}; "
                         "use a larger --lengths value")
    while n < target:
        user += PAD
        n = count_tokens(tok, user)
    return user, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=6)
    ap.add_argument("--lengths", type=str, default=",".join(str(x) for x in DEFAULT_LENGTHS))
    ap.add_argument("--model", type=str, default=P.SETUP_DATA["model"])
    ap.add_argument("--tokenizer-dir", type=str, default=TOKENIZER_DIR)
    args = ap.parse_args()
    targets = [int(x) for x in args.lengths.split(",")]
    P.SETUP_DATA = {**P.SETUP_DATA, "model": args.model}
    print(f"Model under test: {args.model}")

    tok = load_tokenizer(args.tokenizer_dir)
    prompts = {}
    print("Prompts (exact token counts, measured with the unit's own tokenizer):")
    for t in targets:
        user, n = build_prompt(tok, t)
        prompts[t] = (user, n)
        flag = "" if n == t else f"   (could not hit {t} exactly)"
        print(f"  target {t:>3} -> {n:>3} tokens{flag}")

    client = P.Client()
    print("\nCleaning up any existing llm task...")
    P.cleanup(client, ["llm"])
    work_id = P.setup_and_wait(client)
    print(f"   llm task: {work_id}\n")

    results = {t: [] for t in targets}
    firsts = {t: [] for t in targets}   # seconds from sending the prompt to the first word of the answer
    for trial in range(1, args.trials + 1):
        for t in targets:   # round-robin so slow drift cannot favour one length
            user, n = prompts[t]
            answer, finished, paused, first_s = P.run_trial(client, work_id, user, P.STOP_CHARS_DEFAULT)
            verdict = P.classify(answer, P.WEATHER_WORDS)
            results[t].append((verdict, answer))
            if first_s is not None:
                firsts[t].append(first_s)
            ft = f"{first_s:4.1f}s" if first_s is not None else "  -  "
            print(f"[{trial:>2}/{args.trials}] {n:>3} tokens  first word {ft}  {verdict:<31} | {answer[:60]!r}")

    print("\n" + "=" * 90)
    print("SUMMARY - same question, only the total prompt length differs")
    print("=" * 90)
    for t in targets:
        n = prompts[t][1]
        bad = sum(1 for v, _ in results[t] if v != "ok")
        bar = "#" * bad + "." * (len(results[t]) - bad)
        avg = f"{sum(firsts[t]) / len(firsts[t]):4.1f}s" if firsts[t] else "  -  "
        print(f"{n:>4} tokens  {bad}/{len(results[t])} bad  [{bar}]  first word avg {avg}"
              f"{'   <-- 128' if n == 128 else ('   <-- 256' if n == 256 else '')}")
    print("\nIf the bad answers start right after the model's window (128, or 256 for the p256 model), "
          "length alone is the cause.")

    client.send({"request_id": P.new_request_id(), "work_id": work_id, "action": "exit"})
    client.read_one(timeout=5.0)
    print("Done. Restart the Offline Agent app on the CoreS3.")


if __name__ == "__main__":
    main()
