# module-llm

Tools that run **on the Module LLM** (Linux, AX630C), not on the CoreS3. Nothing here is built
into the firmware, which is why it lives outside `firmware/`.

## melotts_finish_helper_v2.c

Watches the LLM's output stream and the audio queue, and tells the CoreS3 when Becky has really
finished speaking (the CoreS3 then opens the follow-up window).

- Declares completion only after the LLM reported `finish:true` **and** the audio queue has stayed
  empty for `SETTLE_MS` (3500 ms). Without that wait it fired between the second-to-last and the
  last sentence, and Becky heard the end of her own answer.
- Prints `DUPLICATE CHUNK` / `REPEATED WORD` when the same word arrives twice in a row (diagnostic for
  the audible "known for for" stutter: one is a transport duplicate, the other the model repeating).

```sh
# on the PC
adb push module-llm/melotts_finish_helper_v2.c /opt/
# on the Module LLM
pkill -f melotts_finish_helper        # -f is required, see below
gcc /opt/melotts_finish_helper_v2.c -lzmq -o /opt/melotts_finish_helper_v2
/opt/melotts_finish_helper_v2 &
```

Check there is exactly ONE process: `ps aux | grep melotts_finish_helper | grep -v grep`.
Needs `libzmq3-dev`.

### Start it automatically on every boot (systemd)

Started by hand, the helper is gone after a Module LLM reboot, and then the CoreS3 only leaves
"Speaking" when its 90-second fallback timer fires (seen in a real log: 90.8 s, versus 6.6 s with the
helper running). Install it as a service instead:

```sh
# on the PC
adb push module-llm/melotts_finish_helper_v2.c        /opt/
adb push module-llm/melotts-finish-helper.service     /opt/
adb push module-llm/install_finish_helper_service.sh  /opt/
adb shell sh /opt/install_finish_helper_service.sh
```

The script compiles the helper, stops any copy started by hand (two copies would both send a finish
message), and enables `melotts-finish-helper.service`. After that:

- `journalctl -u melotts-finish-helper -f` shows its output (replaces the terminal you used to start it).
- `systemctl status melotts-finish-helper` shows whether it is running.
- It is normal to see "Could not discover melotts's work_id" repeating every 5 s while the CoreS3
  app has not started yet: the helper exits and systemd starts it again until the tasks exist.
- The helper survives a CoreS3-only restart: the app re-creates its tasks (llm.1003 -> llm.1008,
  melotts.1004 -> melotts.1009 in a real log), the helper prints "melotts's work_id changed" and
  keeps working, because the llm output stream it listens to stays the same.
- To update the helper later: push the new `.c`, run `sh /opt/install_finish_helper_service.sh` again.

## probes/ (Python, run on the Module LLM)

The three scripts import each other, so keep them in the same folder (`/opt`). Each one exits any
existing llm task, so **leave the Offline Agent app on the CoreS3 first** and restart it afterwards.
Run long ones detached so a dropped terminal does not kill them:

```sh
adb push module-llm/probes/*.py /opt/
nohup python3 -u /opt/probe_sensor_prompts.py > /opt/sensor_prompts.log 2>&1 &
```

| script | answers |
|---|---|
| `probe_prompt_derail.py` | replays exact prompts N times and classifies how each answer STARTS (derail / ok) |
| `probe_prompt_length.py` | same question padded to exact token counts; locates the prompt-length cliff. `--model` / `--tokenizer-dir` to test another model |
| `probe_sensor_prompts.py` | does the answer actually use the attached sensor/GPS reading? compares wordings |

## What these tools established (keep in mind when changing prompts)

- **Prompt window is 128 tokens** for `qwen2.5-1.5B-Int4-ax630c` (system prompt + chat template + user
  text). 129+ tokens derails the model from the first token, every time (30/30); 128 and below is fine.
  The firmware therefore estimates tokens and trims (see `prompt_user_token_budget()` in
  `firmware/main/hal/offline_agent_module.cc`). The docs' "128 context window" means this, not the
  conversation length.
- **Turns are independent**: the unit keeps no conversation memory. Anything Becky should "remember"
  has to be put into the prompt by the firmware, inside the token budget.
- **The unit does not stop on `<|endoftext|>`** (only on `<|im_end|>`), so the model can keep going
  into an unrelated "document". The firmware cuts the answer at that marker.
- **Sampling flags** (`enable_temperature`, `enable_repetition_penalty`, ...) are off by default and
  are honoured when sent in `llm.setup`; `post_config.json` is overridden by the request.
- **Pitfall:** `pkill name` silently matches nothing for names longer than 15 characters; use `-f`.
- The Module LLM clock is wrong (broken RTC); trust line order in `journalctl`, not timestamps, and use
  `journalctl -u llm-llm -b`.
