# OKILTV documentation for AI agents

This directory is the English, task-oriented documentation for agents working on
OKILTV. Start with the repository [AGENTS.md](../AGENTS.md), then read the relevant
pages below before changing code. These pages explain the current implementation,
its boundaries and where to make changes; they are not a release history.

## Reading map

| Task | Read |
|---|---|
| Understand startup, ownership or dependencies | [Architecture](architecture.md) |
| Change source import, channels, groups, programme data or local EPG search | [Sources and EPG](sources-and-epg.md) |
| Change Live, catch-up, timeshift, recording or multiview | [Playback](playback.md) |
| Change movie/series libraries, episodes, VOD sessions, progress or uploaded subtitles | [VOD](vod.md), [uploaded subtitles](vod.md#uploaded-subtitles) |
| Change QML, focus, shortcuts, overlays or appearance | [UI and interaction](ui-and-interaction.md) |
| Change settings, persistence, migrations or protected data | [Settings and storage](settings-and-storage.md) |
| Build, validate, diagnose CI or package the application | [Build and testing](build-and-testing.md) |

For a cross-cutting change, read each affected page. For example, a new playback
setting involves settings/storage, playback, UI and validation.

## Documentation responsibilities

- Keep these documents in English and update the affected pages in the same
  change as the implementation. Update behavior, ownership, APIs, persistence,
  settings, UI contracts, commands and validation requirements when they change.
- Keep the permanent contracts in [AGENTS.md](../AGENTS.md) consistent with these
  pages. Keep this index and relative links current when adding or moving pages.
- Edit the relevant permanent section. Do not append “Latest Delta”, “Previous
  Delta” or historical changelog sections.
- Use source links and symbol names instead of line numbers, copied declarations,
  fixed test counts or repeated release versions. The version comes from
  [qt/CMakeLists.txt](../qt/CMakeLists.txt); available presets come from
  [CMakePresets.json](../CMakePresets.json).
- Distinguish implemented behavior, planned work and manual acceptance still
  outstanding. Passing a build does not establish playback quality on Windows.
- When code and documentation disagree, inspect the implementation and relevant
  tests, preserve explicit project contracts, and resolve the discrepancy in the
  affected documentation. Do not silently treat a design proposal as implemented.
- Never include provider credentials, resolved stream URLs, private captures or
  secrets in documentation, examples, logs or committed fixtures.

## Related material

[AGENTS.md](../AGENTS.md) remains the authoritative repository instruction and
detailed contract reference. [The root README](../README.md) is the user-facing
overview. The [.project directory](../.project) contains focused design and
investigation documents; some describe proposals and historical verification, so
check their status against code before applying them.

Relevant supporting references include the [source feature contracts](../.project/source-feature-parity.md),
[playback ownership map](../.project/playback-refactoring.md),
[provider data protection](../.project/security-storage.md),
[VOD design and acceptance status](../.project/OKILTV_ARCHITEKTURA_VOD.md),
[VOD storage contract](../qt/src/core/vod/storage/README.md),
[CI setup](../scripts/ci/README.md) and
[Linux UI harness](../ui-tests/linux-ui-tests.md).

Existing images in [screenshots](screenshots) are visual references, not an
authoritative description of current behavior.

The [EPG search validation record](epg-search-validation.md) records actual local
checks and outstanding Windows/manual acceptance for the programme-search feature.
