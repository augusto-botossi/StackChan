"""
probe_location_wording.py

How should the position reading be written so the model actually uses it?

In probe_system_prompt.py, "How about our current location?. Current position: latitude is 50.2575
degrees north, longitude is 8.6429 degrees east." was answered WITHOUT the coordinates 2 to 3 times in 5
("Great to hear you're with me!"). The system prompt did not matter, so the suspect is the attached
sentence (and the way the question is asked). This probe keeps the current system prompt and compares
several wordings of that sentence, over several ways of asking where we are.

The last variant tests an idea for later: if Becky could look the coordinates up in a place list stored
inside her, the sentence could carry a town name ("We are near Frankfurt, Germany ..."). It uses a real
city only as an example, to see whether the model then says the name.

No speech: it talks only to the llm unit (TCP port 10001). Answers are stopped after about 110 characters.

BEFORE RUNNING: leave the Offline Agent app on the CoreS3 (go back to the launcher): the probe exits the
llm task that exists on the unit. Restart the app afterwards. Needs probe_prompt_derail.py in the same folder.

    adb push probe_location_wording.py /opt/
    python3 -u /opt/probe_location_wording.py > /opt/location_wording.log 2>&1
    python3 -u /opt/probe_location_wording.py --trials 10 --only current,we_are    # more samples, fewer wordings
    # The town lookup sentences of the firmware (use the town Becky actually names for your position):
    python3 -u /opt/probe_location_wording.py --only fw_town_only,fw_town_near --town "Oberursel, Germany" > /opt/location_town.log 2>&1

About 30 minutes with the default 5 trials (7 wordings x 5 questions x 5 trials = 175 answers).
"""

import argparse
import re

import probe_prompt_derail as P

# Same text as kSystemPrompt in offline_agent_module.cc. Keep them in step.
SYSTEM_PROMPT = ("You are Becky, a friendly voice assistant. Do not use contractions. "
                 "Answer in at most three short sentences unless asked for more. "
                 "You have sensors; use the readings given with a question.")

LAT = "50.2575 degrees north"
LON = "8.6429 degrees east"

# The ways someone asks. A leading space on every one, as ASR results arrive in the app.
QUESTIONS = [
    " How about our current location?.",
    " Where are we right now?.",
    " Where are we?.",
    " What are our coordinates?.",
    " Do you know where we are?.",
]

# name -> function(question) -> the whole prompt sent to the llm.
VARIANTS = {
    # What the firmware sends today.
    "current":     lambda q: f"{q} Current position: latitude is {LAT}, longitude is {LON}.",
    # Natural first-person statements.
    "we_are":      lambda q: f"{q} We are at latitude {LAT} and longitude {LON}.",
    "our_position": lambda q: f"{q} Our position is latitude {LAT} and longitude {LON}.",
    "gps":         lambda q: f"{q} The GPS says we are at {LAT} and {LON}.",
    # Same statement, but BEFORE the question (the name prefix worked best in front).
    "reading_first": lambda q: f" We are at latitude {LAT} and longitude {LON}.{q}",
    # Tells the model what to do with it.
    "say_it":      lambda q: f"{q} Current position: latitude is {LAT}, longitude is {LON}. Tell the user these coordinates.",
    # The later idea: a town name from a stored place list, with the coordinates kept.
    "with_town":   lambda q: f"{q} We are near Frankfurt, Germany, at latitude {LAT} and longitude {LON}.",
}

# What the firmware sends now when places.bin knows the town (hal_offline_agent_callbacks.cpp). The town is
# filled in by main() from --town ("Name, Country"); within 30 km the sentence says "near", beyond it
# "the nearest town is".
TOWN_TEXT = ["Frankfurt, Germany"]
VARIANTS["fw_town_near"] = lambda q: (f"{q} Current position: latitude is {LAT}, longitude is {LON}. "
                                      f"We are near {TOWN_TEXT[0]}. Tell the user the town and these coordinates.")
VARIANTS["fw_town_far"] = lambda q: (f"{q} Current position: latitude is {LAT}, longitude is {LON}. "
                                     f"The nearest town is {TOWN_TEXT[0]}. Tell the user the town and these coordinates.")
# Since 2026-10-09 the firmware sends only the town unless the user asks for coordinates/latitude/longitude/GPS.
VARIANTS["fw_town_only"] = lambda q: f"{q} We are near {TOWN_TEXT[0]}. Tell the user the town."
# Becky once added "in Bavaria, close to the river ..." (wrong: Friedrichsdorf is in Hesse). Ways to stop the
# model from adding facts of its own about the town:
REGION_TEXT = [""]   # set by --region, e.g. "Hesse"
VARIANTS["fw_town_only_strict"] = lambda q: (f"{q} We are near {TOWN_TEXT[0]}. "
                                             "Tell the user only the town and country. Do not add other details.")
VARIANTS["fw_town_region"] = lambda q: (f"{q} We are near {TOWN_TEXT[0].split(',')[0]}, {REGION_TEXT[0]}, "
                                        f"{TOWN_TEXT[0].split(',')[-1].strip()}. Tell the user the town.")
VARIANTS["fw_town_region_strict"] = lambda q: (f"{q} We are near {TOWN_TEXT[0].split(',')[0]}, {REGION_TEXT[0]}, "
                                               f"{TOWN_TEXT[0].split(',')[-1].strip()}. "
                                               "Tell the user only the town and country. Do not add other details.")
VARIANTS["fw_no_town"] = VARIANTS["say_it"]   # what the firmware sends without the card (same as say_it)

COORDS = re.compile(r"50\.?\s?2|8\.?\s?64|\b50 degrees|\b8 degrees")
TOWN = re.compile(r"(?i)frankfurt")
# Facts the model adds on its own about the place (regions, rivers, neighbours, ...). Wrong ones are the problem.
EXTRAS = re.compile(r"(?i)bavaria|bayern|hesse|hessen|river|state of|region|county|district|province|"
                    r"close to|north of|south of|east of|west of|located|known for|capital|population|"
                    r"suburb|outskirts|famous|mountain|lake|forest")
STOP_CHARS = 110


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", type=str, default="qwen2.5-1.5B-p256-ax630c")
    ap.add_argument("--trials", type=int, default=5)
    ap.add_argument("--only", type=str, default="")
    ap.add_argument("--region", type=str, default="Hesse",
                    help="region of the town, for the fw_town_region variants")
    ap.add_argument("--chars", type=int, default=220,
                    help="stop each answer after this many characters (110 cut off the extras)")
    ap.add_argument("--town", type=str, default="Frankfurt, Germany",
                    help='town the lookup would give for these coordinates, "Name, Country"')
    args = ap.parse_args()
    TOWN_TEXT[0] = args.town
    REGION_TEXT[0] = args.region
    global TOWN
    TOWN = re.compile("(?i)" + re.escape(args.town.split(",")[0].strip()))
    names = [n for n in VARIANTS if not args.only or n in args.only.lower().split(",")]

    client = P.Client()
    P.cleanup(client, ["llm"])
    P.SETUP_DATA = {**P.SETUP_DATA, "model": args.model, "prompt": SYSTEM_PROMPT}
    work_id = P.setup_and_wait(client)
    print(f"Model under test: {args.model}; {len(names)} wordings x {len(QUESTIONS)} questions x "
          f"{args.trials} trials\n", flush=True)

    # (variant, question index) -> list of (coords?, town?, answer)
    results = {(n, qi): [] for n in names for qi in range(len(QUESTIONS))}
    for trial in range(1, args.trials + 1):
        for qi, q in enumerate(QUESTIONS):
            for n in names:       # interleaved, so a slow drift cannot favour one wording
                prompt = VARIANTS[n](q)
                ans, _fin, _pau, _first = P.run_trial(client, work_id, prompt, args.chars)
                results[(n, qi)].append((bool(COORDS.search(ans)), bool(TOWN.search(ans)), ans))
        print(f"   trial {trial}/{args.trials} done", flush=True)

    print("\n" + "=" * 104)
    print("SUMMARY  answers that state the coordinates (any of the numbers), out of "
          f"{args.trials} per cell")
    print("=" * 104)
    head = f"{'wording':<14}" + "".join(f"{'Q' + str(i + 1):>7}" for i in range(len(QUESTIONS))) + \
           f"{'total':>10}{'town said':>11}"
    print(head)
    for n in names:
        row, tot, towns, cnt = f"{n:<14}", 0, 0, 0
        for qi in range(len(QUESTIONS)):
            rs = results[(n, qi)]
            ok = sum(1 for r in rs if r[0])
            tot += ok
            towns += sum(1 for r in rs if r[1])
            cnt += len(rs)
            row += f"{ok:>5}/{len(rs)}"
        row += f"{tot:>6}/{cnt:<4}" + (f"{towns:>7}/{cnt}" if n in ("with_town", "fw_town_near", "fw_town_far", "fw_town_only", "fw_town_only_strict", "fw_town_region", "fw_town_region_strict") else f"{'-':>8}")
        print(row)
    print("\nQuestions:")
    for i, q in enumerate(QUESTIONS):
        print(f"  Q{i + 1}: {q.strip()}")

    print("\nANSWERS THAT ADD FACTS OF THEIR OWN (region, river, neighbours ...; the region we gave does not count)")
    own_region = REGION_TEXT[0].lower()
    for n in names:
        bad = []
        for qi in range(len(QUESTIONS)):
            for _c, _t, ans in results[(n, qi)]:
                hits = [m.group(0) for m in EXTRAS.finditer(ans)]
                if "region" in n and own_region:
                    hits = [h for h in hits if h.lower() != own_region]
                if hits:
                    bad.append((qi, hits, ans))
        total = sum(len(results[(n, qi)]) for qi in range(len(QUESTIONS)))
        print(f"  {n:<24} {len(bad)}/{total} answers with extras")
        for qi, hits, ans in bad[:6]:
            print(f"      Q{qi + 1} {hits} -> {ans[:160]!r}")

    print("\nANSWERS WITHOUT THE COORDINATES (first characters):")
    for n in names:
        for qi in range(len(QUESTIONS)):
            for coords, town, ans in results[(n, qi)]:
                if not coords and not (n in ("with_town", "fw_town_near", "fw_town_far", "fw_town_only", "fw_town_only_strict", "fw_town_region", "fw_town_region_strict") and town):
                    print(f"  [{n}] Q{qi + 1} -> {ans[:90]!r}")

    client.send({"request_id": P.new_request_id(), "work_id": work_id, "action": "exit"})
    client.read_one(timeout=5.0)
    print("\nDone. Restart the Offline Agent app on the CoreS3.")


if __name__ == "__main__":
    main()
