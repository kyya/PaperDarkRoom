---
name: release-notes
description: Use when writing or editing a GitHub Release body (release notes / changelog for a release). Trigger words (English and Chinese, since this project's release notes are written in Chinese for its Chinese-speaking users): release note, release notes, gh release, gh release create, gh release edit, changelog for release, 发版, 发新版, 写发布说明, 发布说明, 版本说明, 这次发布的说明, 写 changelog. Trigger when the user is about to publish a new version, or wants to write/edit any end-user-facing Release body — do not confuse this with writing commit messages or PR descriptions, which are for developers and follow different rules.
---

# Release Notes SOP

## Core principles

1. **The audience is the product's users, not developers.** Release notes are written for players/end users: only describe changes they can actually perceive, in their language. For example, write "the screen no longer flashes black at random" rather than "ration quality refreshes."

2. **No technical detail, ever.** Internal function names, refresh modes, algorithm names, refactor explanations, PR numbers, commit scopes — all of that belongs to commit messages and PR bodies, not release notes. Before writing a line, ask: is there a word in this sentence a user couldn't understand without reading the code? If so, cut it or rephrase it.

3. **Look at history before writing anything.** Run `gh release list` to pull the recent version list, then `gh release view <tag>` on the last 2-3 versions to read their bodies, and follow their:
   - Language (if the project's release note history is written in Chinese, keep writing in Chinese — don't let the global "commits/PRs are English" rule bleed in here; **that rule does not cover release notes**. Release notes are for users, so use whatever language they're used to reading.)
   - Tone
   - Punctuation style
   - Paragraph structure (e.g. a fixed "flashing/install" section, a fixed divider placement)

4. **Keep it short.** Cap the change list at 5-8 lines, one user-perceivable change per line. When there's no historical convention to follow, fall back to conventional-changelog's minimal style (grouped by type, one line each), but still prioritize rewriting from the user's point of view — don't just copy the raw commit text over.

5. **Copy fixed sections structurally, not verbatim.** Install instructions, flashing steps, checksums, download links, and other practical sections that already exist in past versions should keep their structure — only update the version number, file-name-to-hash mappings, and other fields that actually change per version.

6. **Never rename attachments.** Release asset file names stay exactly as the build script produces them — don't rename them "to look nicer." Downstream tooling (flashing scripts, auto-updaters, etc.) may fetch them by a fixed file name.

## Workflow

1. **Establish the tone**: `gh release list` to see recent tags → `gh release view <previous tag>` (and the one before that) to read their bodies and pin down language, tone, and structure.
2. **Extract the changes**: go through the PR list / commit list for the version being released (`git log <previous tag>..HEAD --oneline` or `gh pr list --state merged`) one by one, and for each ask: "Can a user notice this without reading the code? If so, what would they feel?" Keep only what's perceivable — internal refactors, pure code cleanup, and test changes are never written in.
3. **Write the body in the historical format**: apply the language/tone/structure locked in during step 1, compress the extracted changes into at most 5-8 lines, and keep the fixed install/flashing/checksum sections with their version numbers and hashes updated.
4. **Review before shipping**: show the draft to the user first, or paste the published body immediately after release, so they can confirm no technical jargon slipped in and the tone matches past releases.

## Before/after examples

The right-hand column is real Chinese release-note copy — kept as-is below since these are genuine instances of what this project's Chinese-language release notes look like, not translation exercises.

| Scenario | Wrong (commit-flavored) | Right (user-perceived, in this project's actual release language) |
|---|---|---|
| EPD refresh strategy tweak | `perf(epd): ration quality refreshes, deep-clean on idle (#30)` | 屏幕不再时不时整片黑闪一下。清屏改在你没盯着看的时候悄悄做完。 |
| Economy settlement fix | `fix(economy): settle partial crews when inputs run short (#26)` | 原料不够时，队伍不会再莫名其妙卡住不干活了。 |
| Version bump commit | `chore: bump version to 0.16.0 (#29)` | (a purely internal maintenance change the user can't perceive — leave it out of release notes entirely) |

Rule of thumb: if a line would look perfectly normal pasted onto GitHub as a commit message or PR title, it doesn't belong in release notes as-is — either translate it into a user-perceived description of the experience, or decide the user can't perceive it at all and cut it.
