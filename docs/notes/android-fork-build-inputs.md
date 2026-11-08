# Building an Android fork with authorized inputs

The public source tree does not contain Disc 1's `default.xex` or generated PPC sources. The official Android workflow stages the XEX privately, builds the host recompiler, generates PPC code, and only then compiles and packages the ARM64 runtime. A missing input is a blocked build, not a successful APK.

Choose one authorized input source in the fork's repository settings:

- Set the **Actions secret** `LO_BUILD_XEX_URL` to a private HTTPS download URL for your own Disc 1 `default.xex`. The existing provisioning script reads it without printing the URL or content.
- Alternatively, keep the XEX in an authorized private GitHub repository. Set the **Actions secret** `LO_BUILD_INPUT_KEY` to a read-only deploy key authorized on that repository. Set the **Actions variables** `LO_BUILD_INPUT_REPOSITORY` (`owner/name`) and `LO_BUILD_INPUT_REF` (a pinned commit) to select it.

The URL takes precedence when both secrets are configured. With neither secret the workflow stops before installing the expensive toolchain and states exactly what is missing.

When no repository variables are provided, the official private repository and pinned commit remain the defaults. Access to the public fork does not grant access to the maintainer's private inputs. Do not copy, expose or request the maintainer's credentials.

For a private input repository, store the XEX at `default.xex`. Optional `android/libdxcompiler.so` is reused only when its hash matches the official pin; otherwise Android DXC is built from source. Optional `android/debug.keystore` preserves the project's signing identity when its use is authorized. A newly generated fork signing key cannot update an installed APK signed by another key.

Keep game inputs, private URLs, keys and keystores out of public commits, build logs and artifacts. The workflow uploads the APK and test reports, not the input checkout or generated source tree. After a successful build, signature, ABI, integrity and actual-device acceptance remain separate checks.

GitHub may separately suspend Actions on a fork because of usage volume. A repository maintainer must re-enable Actions before a push or manual dispatch can run. Provisioning an XEX does not remove that platform block.

## Steprant Android 1.8 packaging

The current default branch is `steprant/v1.8`. On this branch, `android-apk.yml` selects version **1.8** and the Steprant package, launcher label and icon; the CI artifact APK is `LostOdyssey-Steprant-v1.8.apk`. Historical integration and candidate branches retain their own 1.5, 1.6 and 1.7 build identities.

The [Android APK build 42482559698](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/42482559698) succeeded from commit `6b5fe4873dc03ca703a947477d8c32287ce0d3b2`. The [Steprant release workflow](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/42486130936) published it as `LostOdyssey-Steprant-v1.8-20261108.apk`; see the [1.8 release notes and checksum](steprant-1.8-release.md). The original signing identity is retained so the APK can update earlier Steprant builds. New builds have their own checksums.

`steprant-draft-release.yml` is the Android-only Steprant publication flow: it takes a finished Android APK artifact, attaches it to the 1.8 release and uses `docs/notes/steprant-1.8-release.md` for its notes. Despite the historical filename, the current workflow publishes the release.

`release.yml` is the inherited **complete multi-platform** release pipeline. Its `GH_REPO` now resolves to `github.repository`, so a fork cannot accidentally target Frank's releases. Automatic `v*-steprant.*` tags are excluded from this pipeline: Steprant Android releases contain a separately verified Android APK, rather than the pipeline's mandatory Windows/Linux/macOS/Android package set. Ordinary upstream version tags and explicit manual multi-platform rehearsals keep their existing builds and publication checks. This routing does not provide the fork with access to upstream private inputs.

Do not rerun the old tag's failed workflow expecting changed branch files: its checkout still uses the original tag. No existing release tag is moved. Use the corrected branch for future builds, with authorized inputs configured.

## Workflow failures reviewed on 5 November 2026

Historical checkpoint: the failures below remain failed runs. The successful
8 November 1.8 build and publication above supersede the old missing-input block.

| Workflow and evidence | Cause and current state |
|---|---|
| [Android APK run 41909714370](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/41909714370) | Stops at **Select authorized private build input** because neither input secret is available. At that checkpoint, toolchain installation, generated game code and APK compilation did not run. The 8 November 1.8 APK build succeeds; this earlier run remains failed. |
| [Release run 41910592756](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/41910592756) | Inherited `GH_REPO` pointed at upstream; release-note extraction also lacked an exact `v1.5-steprant.20261104` changelog heading. The repository target and heading are corrected. Android-only Steprant tags now use their separate release flow. No failed historical run is relabeled successful. |
| [Integration run 41905980122](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/41905980122) | Host tests lacked Pillow and NumPy. The existing dependency fix installs `python3-pil` and `python3-numpy`; the later [integration run 41909714384](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/41909714384) succeeds. No further test removal or bypass is needed. |
| [TV diagnostic run 41905461394](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/41905461394) | Java compilation called private `wakePhone()`. The current probe uses the package-visible `phoneInteraction()` entry point instead; the later [TV run 41910592603](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/41910592603) succeeds. This task does not modify that runtime implementation. |
