# General guidelines (gd-tool-02 / dream-charter-engine)

The owner's rules for every task here, for people and AI agents alike
(given 2026-11-19 with the full-rework specs; adapted from prj-gd-engine's
`docs/knowledge/general_guidelines.md`).

## Before you start

- **Guidelines.** Read this file, [product_spec.md](product_spec.md) and the
  [backlog](backlog.md) before starting.
- **Backlog (extreme importance).** [backlog.md](backlog.md) is the single
  backlog. Read and update it **at the start and at the end of every task**.
  Keep IDs stable.
- **Branching.** Work in the current branch (`spike/20261106-discovery`). Pull
  requests are a gate the owner triggers.
- **Time to finish.** Give an estimate when you start.
- **Questions.** Ask whenever you are not 100% sure, with the interactive
  question tool.
- **System and app.** Understand the project before changing it: the root
  `README.md` (the ai_charter engine), `gd-tool-02/README.md` and these docs.

## While you work

- **Memory.** Anything needed outside the current context goes in a markdown
  file (these docs or the backlog), never only in a conversation.
- **Context.** The codebase is large: compact the conversation when needed.
- **Multi-platform.** Test on as many systems as possible before finishing.
  Regular tests run mainly on Linux (here: a Fedora container via podman,
  see the README).
- **Tests.** Test what you did; people use the app a lot.

## When you finish

- **Codebase.** Commit and push finished work. The owner says when it is time
  for a pull request.
- **Version.** At PR time, bump the version of what changed
  (`gd-tool-02/VERSION`, and the engine's `pyproject.toml` when the engine
  changed).
- **Packages.** Build everything locally after a bugfix or improvement. The
  prj-gd-engine rules about the universal launcher (modules, ISOs, keeping the
  current and previous launcher build) apply to that repo, not to this one.
