# Project instructions: De-Honk

Paste everything below the line into the shared project's instructions.

---

## What this project is

De-Honk is an audio plug-in by Samuel Justice. It sits on the master bus before the limiter and removes the low-mid honk that bass enhancers, saturators and other harmonic generators leave in a mix, without removing the bass or the body. RBass is the most common cause and was measured first, but it is not the only target. It is for music and effects composites (think trailers). The mixes will have dialogue in them, but it is not a dialogue tool. The interface is a low-poly 3-D scene in soft pastels: a goose on a pond that eats whatever each module is removing. Never name a game in code, interface or documents.

The specification is `dehonk/SPEC.md` in the repository. Read it before doing anything. If a request conflicts with the spec, say so and ask; if the spec changes, update the file in the same change.

## Repository

- GitHub: `https://github.com/samueljustice/plugins` (public). This is the only repository for plug-in code. Do not create a new one.
- All De-Honk code lives in `dehonk/`. The other folders are other plug-ins. Do not change them, and do not propose changes to them, unless Samuel asks.
- Shared code is in `common/` (update check), `cmake/` (AAX) and `bundle/` (release scripts, combined installer). Use what is there. Changes to these affect every plug-in, so ask first.
- Branch from `origin/main` and name the branch `dehonk/<what-it-does>`. Open a pull request into `main`. Pull requests are merged with merge commits, not squashed. Other people and sessions merge to `main` often, so sync with `main` before testing and before merging.
- Never commit to `main` directly, never force-push, never push tags (see Releases).

## Building

- JUCE 8.0.8, CMake. Every plug-in's `CMakeLists.txt` expects JUCE at `../SDK/JUCE`. `SDK/` is git-ignored: put your own copy there, or symlink one in. Never commit it.
- Follow an existing plug-in for layout: `subbertone/` for the folder structure, preset manager and OpenGL view, `swooshbuckler/` for tests, `reversinator/` for the release workflow and CMake settings.
- Formats: VST3, AU, AAX, Standalone. macOS and Windows. Mono and stereo.
- CMake must set a macOS deployment target of 11.0 and a static MSVC runtime (the Windows installer ships no VC++ redistributable).
- Vendor name is `Sammy J`.
- Avid's AAX SDK must never be committed to this repository. It is under Avid's licence. CI fetches it from a private repository; locally, pass `-DSAMMYJ_AAX=ON` and, if you have the SDK, `-DAAX_SDK_PATH=...`.
- Test audio from real sessions is client material. Keep it out of the repository, in a git-ignored folder.

## Engineering rules

- Nothing in `processBlock` may allocate, lock or make a system call.
- The DSP core is plain C++ with no JUCE dependency, written once and shared by the offline test harness and the plug-in.
- It runs on master buses for whole sessions. Hold to the CPU targets in the spec and run the CPU benchmark before every pull request.
- Tests live in `dehonk/Tests`. A pull request that changes DSP includes the test results in its description: say plainly what passed, what failed and what was not run.
- Known host behaviour to design for: AU hosts call `reset()` on the UI thread when bypass is engaged, at the same time as audio is rendering; Logic addresses AU parameters by index, so adding or removing a parameter after release shifts automation.

## Who builds what

To keep cost down the work is split between two models. The table is in section 8.1 of the spec. In short:

- **Opus 5.5** writes the bulk: plug-in shell, parameters, presets, test harness, knobs and layout, installer, CI, docs.
- **Fable 5.2** writes the DSP core and the whole visualiser (spectrum display, pond, goose), tunes against the test corpus, and reviews every Opus change.

If you are working on something outside your model's column, stop and say so.

## Releases

Releases go through the repository's release process. Do not build installers by hand or upload files to a release yourself.

- Each plug-in has a workflow, `.github/workflows/build-release_<plugin>.yml`. De-Honk's is `build-release_dehonk.yml`, copied from Reversinator's. A tag `dehonk-vX.Y.Z` on `main` builds, signs and notarises macOS and Windows and publishes the release.
- **Only Samuel pushes tags and signs AAX.** He batches releases and signs with a hardware iLok. Your job ends at: tests pass, dry run passes, pull request merged.
- Dry run: run the workflow by hand on your branch (`workflow_dispatch`). It does the full build without publishing. Do this before asking for a merge that is meant to be released.
- The version lives in `project(... VERSION)` in `dehonk/CMakeLists.txt`. The workflow fails if the tag disagrees.
- Every update updates its documentation in the same pull request:
  1. `dehonk/CHANGELOG.md` and `dehonk/RELEASE_NOTES.md`, headed with the new version. The release notes become the release text.
  2. `dehonk/README.md`, including screenshots of anything that looks or works differently.
  3. De-Honk's section in the root `README.md`, which is also the website (`samueljustice.github.io/plugins`).
  4. Run `bundle/check-release-docs.sh dehonk <version>`. It must pass.
- First release only: add De-Honk to `bundle/plugins.txt` and `bundle/userdata.txt`, add the `dehonk-v` prefix to the list under "Making a release" in the root README, and wire in the shared update check from `common/` with that tag prefix.
- Do not enable or run the combined installer workflow (`build-release_bundle.yml`). Samuel does that.
- Download links in READMEs point to the combined installer only, never to a single plug-in's files.

The full checklist is in the root `README.md` under "Making a release".

## Writing

This applies to the README, landing page, release notes, tooltips and labels.

- Plain paragraphs. Say what the thing does. No sales adjectives, no pitch followed by a bullet list of features.
- Explain each control on its own terms. Never refer to earlier versions in interface text ("the old mode", "the 1.0 sound").
- Credit Samuel Justice. Do not brand the plug-in as Sweet Justice.
- RBass is a Waves product. Name it where it helps a user understand the use, and do not suggest any affiliation.
- Interface text must fit inside its bounds at every window size.

## Working with Samuel

- Ask before anything outward-facing or hard to undo: merging, enabling workflows, deleting branches, changing repository settings.
- For documentation changes, check your own work and stop. Do not start a separate review pass.
- Report results as they are. If something was not tested, say it was not tested.
