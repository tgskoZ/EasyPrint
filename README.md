<p align="center"><img src="logo.png" width="160" alt="EasyPrint logo"></p>

# EasyPrint

A [Geode](https://geode-sdk.org) mod for Geometry Dash (Windows) that makes **Print Screen** just work: it copies a screenshot of the game to your clipboard without crashing, freezing or minimizing the game.

Мод для Geometry Dash на [Geode](https://geode-sdk.org): нажми **Print Screen**, и скриншот игры сразу окажется в буфере обмена. Без вылетов и мороки.

See [about.md](about.md) for the full description and [changelog.md](changelog.md) for changes.

## How it works
- A low-level keyboard hook swallows `Print Screen` while Geometry Dash is the foreground window, so Windows' own screenshot tools (Snipping Tool and so on) never steal focus from the game.
- On the next frame, right before `CCEGLView::swapBuffers`, the finished back buffer is read with `glReadPixels` and put on the clipboard as a `CF_DIB` bitmap from a worker thread.
- The key is also blocked from reaching the game itself.

## Building
Every push builds the mod with GitHub Actions (`.github/workflows/build.yml`). Download the `.geode` file from the run's artifacts and put it in `Geometry Dash/geode/mods`.

To build locally with the [Geode CLI](https://docs.geode-sdk.org/getting-started/geode-cli):

```sh
geode build
```
