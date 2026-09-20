# agents/

Instructions for working on diskOS with an AI coding agent.

`AGENTS.md` in this folder is a ready-made project brief: the hardware facts that are easy to
get wrong, the repo layout, the device-safety rules, and the "verify, do not guess" discipline
that keeps this project honest and the device un-bricked.

## How to use it

- Copy `agents/AGENTS.md` to your agent's project-instruction file at the repo root (many agents
  read an `AGENTS.md` there), or point your agent at it and have it read that file first.
- Or paste the contents in as system/context before you start a session.

The point is to give the agent the same guardrails we use: check `docs/HARDWARE.md` before
claiming anything about the hardware, never assert device behavior from memory, keep the
fail-closed boot contract intact, and treat a flash as recoverable but serious - not guaranteed.

## Good first tasks

- Read `docs/HARDWARE.md` and the installer's `README.md`, then ask the agent to explain the
  install flow back to you - a quick check that it has the right mental model.
- Add support/validation for a new stock firmware version (must be flash-tested on real
  hardware before it is declared supported).
- Improve error messages / add error codes for failure modes not yet covered.
