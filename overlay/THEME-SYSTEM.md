# Theme system

ProsperoRadio Modernized 2.2 adds three static 1920x1080 radio-front themes:

- `radio-walnut.tga` — warm wood and amber hi-fi finish.
- `radio-silver.tga` — classic brushed studio metal.
- `radio-graphite.tga` — darker contemporary studio finish.

The selected theme is controlled by `RadioApp`, displayed through the Theme control, and persisted to `/download0/prosperoradio-theme.txt`.

The dynamic UI remains renderer-safe and uses only the bitmap font faces already packaged by the upstream project. The backgrounds carry the material, lighting and cabinet details so RmlUi does not have to reproduce them through CSS effects.
