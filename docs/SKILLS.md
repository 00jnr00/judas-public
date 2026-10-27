# Skills

A skill is a Markdown file in a skills dir (`[skills] paths`, the first entry is where new skills are
written). Judas lists skills in the system prompt and loads a body when a trigger word appears in the
user's message or the model calls `load_skill`.

## Frontmatter

```
---
name: deploy
description: one line shown in the skill index
triggers: deploy, ship it
pinned: false
source: agent
version: 1
---
Body in Markdown.
```

- `name`: defaults to the file stem; `[a-z0-9][a-z0-9-]{0,63}`.
- `description`: index line; defaults to the first non-empty line of the body.
- `triggers`: comma list; a skill without triggers never auto-loads (the model can still call `load_skill`).
- `pinned`: true protects the skill from curator rewrites and archiving.
- `source`: who wrote it (`agent`, `human`, or `external:<path>` for installed skills).
- `version`: bumped by `skill_manage` on each patch or rewrite.

## Parked directories

- `.archive/`: skills retired by `skill_manage archive`; never loaded, can be restored by moving the file back.
- `.quarantine/`: installs the guard blocked; never loaded. Review, fix, and move out by hand.

## skill_manage actions

`list`, `view`, `create`, `patch`, `rewrite`, `archive`, `pin`, `unpin`. Writes go through the normal
permission flow and the guard scan.

## Installing an external skill

```
judas skills install <path-to-skill.md> [--force]
```

- The file is scanned by the guard (`skills/guard.h`). Any high finding sends it to
  `<skills_dir>/.quarantine/<name>.md` unless `--force` is given.
- Otherwise it is written to `<skills_dir>/<name>.md`.
- Frontmatter `source:` is set to `external:<absolute source path>`.
- An installed skill with the same name is refused, even with `--force`.
- Prints one line per finding (severity, rule, line, excerpt) and the destination. Exit code 1 when the
  skill was quarantined or refused.
