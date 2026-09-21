# Runnable UI examples

The maintained scenarios and commands are listed in [tests/README.md](../tests/README.md).
They generate local media, playlists and EPG without provider credentials.

- Live playback, channel selection and PiP: `05-channel-selection.py`.
- Overlay inactivity and Guide/Settings navigation: `06-overlay-inactivity.py`.
- Guide group filtering: `07-guide-groups.py`.
- Keyboard navigation with a stationary pointer: `08-stationary-pointer-navigation.py`.
- Appearance preview, Save/discard and reopening: `08-ui-transparency.py`.

Timeshift service/controller regressions run in `OKILTVQtAppTests`; the retired
provider-dependent UI examples are not part of the automated suite.
