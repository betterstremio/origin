# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

Recorded as `web` to mean one custom design language on every device, not a web app. Origin is native C that draws
its own interface with OpenGL ES 2 (ANGLE on Windows) through SDL2. It runs on LG webOS, Android TV, Google TV, Fire TV,
Android phones (beta) and Windows. macOS and Samsung Tizen are planned. It does not follow Material, Fluent, webOS or
Apple conventions. Phones get their own touch screens (`src/screens/phone_*.c`) in the same design language.

## Users

People who already have a Stremio or Nuvio account and their own addons, and watch on a living-room TV (LG webOS,
a Fire TV stick, an Android TV box) with a remote. They left the official Stremio and Nuvio apps because those were
slow. They want to sit down, find something and get it playing quickly. Phone and Windows users matter less than
TV users.

## Product Purpose

Origin is a fast native client for Stremio and Nuvio accounts and the addons they hold: Home, Discover, Library,
Search, title pages and a player. It shows what the user's addons return, adds Netflix-style For You rows worked
out on the device, and plays the stream links the addons give. It succeeds when browsing and playback feel instant
on the weakest supported device.

## Positioning

A native C app with no web view, in place of the slow official clients. Users keep their own Stremio or Nuvio
accounts, libraries and addons, and can keep both signed in and switch between them. The app ships empty: it hosts
and indexes nothing, and has no built-in keys.

## Operating Context

- Viewed from a sofa, across the room, on TVs from 1080p sticks to a 4K OLED (the developer's LG C1, webOS 6.5).
- Driven by a remote: arrows, OK and Back do everything. LG's Magic Remote and a Windows mouse point too, but a
  pointer never moves a row just by hovering.
- Signing in and typing keys happen on a phone: a QR code, or Settings sent from the phone to the TV.
- Recommendations need the user's own TMDB key. Trakt is optional, with the user's own client id.
- Releases go out as GitHub releases on betterstremio/origin. webOS updates also come through the Homebrew Channel
  listing, and the app can update itself from Settings > About.

## Capabilities and Constraints

- Speed comes first. A feature that makes the app slower or heavier undoes the reason it exists. No web views,
  embedded browsers or heavy runtimes. State the startup, memory and focus-to-preview cost of a change up front.
- The Fire TV stick (often 1 GB RAM, slow quad-core) is a required target. Anything that uses memory is gated on
  `plat_mem_total_mb()`, with safe values below about 1200 MB.
- One shared C codebase. Each package carries only its own platform's code (`src/platform/<target>/target.h`).
- Features: Stremio and Nuvio sign-in (QR or email), Nuvio profiles with PINs, For You rows (about 36), a rotating
  billboard of top picks with a preview inside, Top 10 by region, Cinematic or Classic Home layout, previews when
  resting on a card (IMDb clips and trailers, or the title itself), best-stream pick in one press, episodes,
  sources, subtitles and audio, skip intro and recap, styled subtitles and sync, HDR and Dolby Vision where
  supported, P2P playback, Collections imported from a phone, 11 languages.
- Platform-specific: DTS conversion, a USB drive for P2P and phone casting on webOS only. Windows has mouse and
  keyboard, a window-shaped layout and DTS and TrueHD decoded by the app.
- The startup animation's sound stays at 0 whenever the app opens.
- Terminology: Home, Discover, Library (becoming "My List" in 3.0.0), Search, For You, Collections, billboard,
  Cinematic and Classic layouts, Continue Watching.
- Undecided: what the Shows and Movies tabs in the 3.0.0 header will hold beyond Discover's catalogs split by type.
  Whether Nuvio sign-in may be published is an open question about Nuvio's terms.

## Brand Commitments

- Name: Origin, formerly Better Stremio (and Stremio Origin before that). Package id `tv.stremioorigin.app`.
- Logo and art: `docs/logo-pill.png`, `docs/header-rounded.png`, `assets/logo.png`, `assets/splashicon.png`. The start
  screen draws the name with its O as a ring (`src/screens/splash.c`), white on Home's page colour.
- Every public page carries the notice that Stremio and Nuvio are the names of their owners and that the project is
  independent and unofficial. No Stremio or Nuvio logos, including in the startup animation, without written
  permission.
- Netflix's own TV app is the named reference for Home. That covers the hero (height and preview inside it), the
  rows (an open box at the row's start with the row sliding under it, poster cards cut from the wide art) and the
  3.0.0 header (`so/header-PLAN.md`). The For You screens are checked against `so/recs-mockups/A-billboard.png` and
  `B-rows.png`, except the hero, which follows Netflix.
- No focus glare. The one-pass focus shine from 2.1.9 stays.
- Voice: plain and short. Text on the repo and website goes through the avoid-ai-writing skill. Privacy is stated
  directly: no analytics, nothing sent to the developer.

## Evidence on Hand

- Feature list, install steps, device status and privacy notes: `README.md`. Release notes: `docs/releases/`.
- Launch video from 2.0.0: `docs/brag.mp4`, `docs/brag.jpg`. Compatibility notes: `docs/compatibility.md`.
- Website: betterstremio.github.io (source in `so/site`).
- Netflix reference photos: `so/shots/nf-*`. Recs mockups: `so/recs-mockups/`.
- Measured speed figures live in `so/TODO.md` and the release notes.
- Tested on an LG C1 (webOS 6.5), webOS 4.4, the Google TV emulator and one Windows 11 PC. No Fire TV stick or real
  Android TV box has been tested yet. No testimonials, user counts or press exist; do not invent them.

## Product Principles

1. Speed beats features. When a choice costs startup time, memory or focus-to-picture time, lean against it.
2. The weakest device sets the bar. A stick with 1 GB RAM must stay smooth and safe.
3. The remote does everything, from across the room. Pointers and touch are added, never required.
4. The user brings everything: accounts, addons and keys. The app ships empty and sends nothing to the developer.
5. Where the user names Netflix's TV app as the reference, match it closely and check it side by side.

## Accessibility & Inclusion

- 11 languages, including Arabic laid out right to left (not yet on phones).
- Text and focus must read from across a room on TVs as small as a 1080p panel.
- Subtitles: six colours including softer ones, opacity from 100% down to 50%, and sync.
