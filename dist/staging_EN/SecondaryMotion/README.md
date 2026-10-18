# Secondary Motion Manager for Arknights: Endfield

> An unofficial open-source runtime secondary-motion tool and character-profile manager for **Arknights: Endfield**.
> This page is mainly for technical discussion. For installation and normal usage, please read the user guide included in the release package.
> 这个页面主要用于技术说明；安装和实际使用请查看发布包内的使用说明。

This project adds configurable runtime secondary motion to supported characters while preserving the game's normal animation and movement logic. It includes a native injected runtime and an external Windows manager for per-character gait tuning, jump/inertia tuning, presets, hot reload, native-motion amplification, and character-profile maintenance.

The project grew out of a reverse-engineering investigation of Endfield's Unity/IL2CPP animation pipeline. The final approach does **not** replace locomotion or jump clips. It identifies the actively controlled character, resolves a verified chest-bone pair, observes accepted animation states and validated movement signals, computes a bounded secondary-motion target, and writes an absolute quaternion target at verified points in the animation update chain.

> **Unofficial project.** Not affiliated with Hypergryph or GRYPHLINE.

---

## Features

- Per-character enable/disable
- Global no-write switch for returning the game to fully native motion
- Three character motion modes:
  - **Original** — no custom transform writes
  - **Synthetic** — gait-driven secondary motion with optional Jump/Inertia
  - **Amplify Native** — amplifies the character's existing native rotational delta
- Independent Walk / Run / Sprint / Zipline Up, Down, and frequency values
- Asymmetric upper/lower half-cycle amplitudes (`Down = 0` remains symmetric)
- Continuous phase, smoothed gait transitions, and a separate fast return-to-idle path
- Event-driven Jump/Inertia system:
  - exact active-jump arming
  - delayed takeoff response
  - Rising / Apex / Falling phase targets
  - falling-speed acceleration response
  - landing impulse
  - bounded damped landing tail and explicit landing shake
  - automatic Jump direction normalization for Y/Z-axis character rigs
- Character-specific bones, axis/sign, animation whitelist extensions, and motion parameters
- Preset management with tool-folder primary storage and game-folder runtime mirroring
- Revision-based runtime hot reload with applied-revision acknowledgement
- Automatic active-character detection and normalized character IDs
- Fail-closed behavior for unsupported characters, invalid signals, and unsupported clips
- Developer tools for character detection, skeleton scanning, bone selection, axis testing, validation, and profile creation
- English and Simplified Chinese self-contained release packages
- Lightweight external Windows UI; no in-game overlay required

The current product scope focuses on the **actively controlled character**. Background party members are observed only as part of the game's callback environment and are not a visual target of the project.

---

## Reverse-Engineering Overview

The project began by asking why different model modifications could show dramatically different visible secondary motion even when the underlying body animation appeared similar.

That led to static model inspection, IL2CPP metadata analysis, runtime hooks, transform recording, same-rig animation tests, callback diagnostics, movement-signal probes, and controlled write experiments. Several early animation hypotheses were rejected as stronger runtime evidence became available.

The main findings are summarized below.

### 1. Mesh/bone mapping was an important clue

Static inspection of XXMI/EFMI model modifications showed that changing breast-related palette/bone mappings could produce a large visible difference without redesigning the entire rig.

This was useful for locating relevant transform families and motivated direct runtime bone investigation.

One important correction from the research: per-draw-call palette indices are **not global bone IDs**. Labels such as `b21` / `b22` / `b23` / `b24` must be interpreted inside the component or draw call where they were observed.

### 2. The locomotion clips did not contain reusable chest curves

The original plan explored direct animation-clip reuse.

Runtime single-object and same-rig tests were performed on shared locomotion clips. The tested girl/lady clips produced no meaningful chest-bone rotation curves when evaluated directly.

That closed the original “copy the locomotion chest curves” route and supported a later runtime secondary process rather than reusable chest curves embedded directly in locomotion clips.

### 3. Direct runtime Transform control works

A major milestone was proving that writing the relevant chest-bone `Transform.localRotation` at runtime produces visible motion.

Native-motion amplification was also verified. The corrected path operates on a quaternion delta relative to a captured base orientation:

```text
delta  = inverse(base) * current
output = base * delta^K
```

With the corrected quaternion math, `K = 2` produces a true 2x amplification of the rotational delta.

### 4. Update order was the difficult part

Writing the correct rotation once was not sufficient. Later animation/runtime jobs could overwrite it.

The proven synthetic path computes its target around `AnimatorMono.PreLateTick`. Optional later hooks do not advance motion again; they replay the same cached absolute target inside a short verified window.

`AnimatorMono.PreLateTick` is the required hook. Its original game method runs before the custom compute/write path. `NPCCPUAnimator.LateTick` and `ScriptAnimationJobSyncMono.CalcLayerMainStream` are optional reinforcement hooks: if either is unavailable the Runtime continues in a degraded mode, while failure to install the required PreLateTick path disables custom motion safely.

```text
PreLateTick
  -> read current native rotation
  -> compute one absolute target
  -> write and cache target

LateTick / SyncCalc (optional)
  -> replay the cached target only
```

The key reverse-engineering lesson was:

> The problem was not only “what rotation should be written?” but also “where in the game's animation pipeline can that rotation survive?”

### 5. Multiple Animator callbacks required frame-level write deduplication

Every `AnimatorMono` in a scene may fire `PreLateTick`. Applying the same rotational delta from every callback stacks the angle several times in one frame and makes amplitude depend on nearby entities.

The current runtime still observes identity/switch information from every callback, but only the first callback of a Unity frame may compute and write the controlled character's motion:

```text
observe callback / detect character switch
        |
        v
Unity frame already written?
  yes -> return
  no  -> compute and write once
```

This frame deduplication removes scene-dependent callback stacking by construction. The current product does not use party-count or callback-count amplitude compensation.

### 6. Gait is inferred from accepted Animator clips

The runtime samples the controlled character's Animator at 20 Hz and classifies accepted locomotion clips into:

```text
Idle
Walk
Run
Sprint
Zipline
```

The generic classifier is whitelist-based. Skills, attacks, battle actions, unknown specials, and unsupported clips remain native by default. A character profile may provide a bounded `animation_rules` map for verified special locomotion clips.

This lets the secondary-motion system follow the existing locomotion state machine without replacing it.

### 7. Smooth gait motion requires separating gait state from output phase

Synthetic locomotion uses a continuous oscillator with separate upper and lower half-cycle amplitudes:

```text
first half-cycle:  angle(t) = Up(t)   * sin(phi(t))
second half-cycle: angle(t) = Down(t) * sin(phi(t))
```

where:

- `Up(t)` and `Down(t)` are smoothed amplitude envelopes
- `phi(t)` is a continuously integrated phase
- target frequency is also smoothed
- `amplitude_down_deg = 0` means `Down = Up` (symmetric motion)

The phase is not reset when changing gait. A separate fast release is used for transitions back to idle.

### 8. Jump requires event and continuous movement evidence

Clip names alone were not enough to produce reliable jump inertia. Runtime probes established that an active jump has an exact `jump_start` clip edge and a usable `MovementComponent.fallingSpeed` signal:

```text
active jump start: fallingSpeed > 0
apex:             fallingSpeed approaches 0
falling:          fallingSpeed < 0
landing:          accepted landing clip edge
```

The runtime reads `fallingSpeed` from a resolved raw field without calling MovementComponent getters. The signal provider is allocation-free and publishes validated plain native data to the per-frame motion core.

A direct fall/heavy landing has no accepted active `jump_start` epoch. It therefore remains native instead of receiving a fabricated takeoff history.

### 9. Character profiles remain necessary

Different characters use different bone-name families, local axes, and visual lever arms.

Examples found during research include:

```text
breast_R_01_jnt / breast_L_01_jnt
R_breast_01_jnt / L_breast_01_jnt
xiong_R_0_skin_jnt / xiong_L_0_skin_jnt
```

The same source-space angle can produce different visible deformation on different rigs/models. The project therefore uses per-character profiles and per-character tuning rather than one universal global value.

Unsupported characters fail closed and remain native until a verified profile is added.

---

## Runtime Architecture

High-level flow:

```text
Game / Unity / IL2CPP Runtime
        |
        v
Active-character observation
        |
        +--> normalized character ID
        +--> immutable character profile
        +--> verified left/right bone pair
        |
        v
20 Hz signal layer
        |
        +--> accepted gait clips
        +--> jump_start / landing edges
        +--> validated MovementComponent.fallingSpeed
        |
        v
Motion Engine (first PreLateTick callback per Unity frame)
        |
        +--> Original: no write
        |
        +--> Amplify Native (LateTick path)
        |
        +--> Synthetic locomotion
        |      +--> asymmetric Up/Down oscillator
        |      +--> amplitude envelope
        |      +--> frequency smoothing
        |      +--> continuous phase
        |
        +--> Jump/Inertia epoch
               +--> Armed / Rising / Apex / Falling / LandingTail FSM
               +--> phase-guided second-order response
               +--> landing impulse
               +--> explicit damped landing shake
        |
        v
ComposeTargets
current native rotation * dq(axis, angle)
        |
        v
SafeSetLocalRotation + cached absolute target
        |
        v
Optional verified-window absolute-target replay
```

The injected runtime is `sbm.dll`.

Core technologies:

- C++
- IL2CPP metadata/reflection and raw-field resolution
- MinHook
- GameAssembly runtime hooks
- Unity `Transform.localRotation`
- quaternion composition and replay
- immutable JSON configuration snapshots
- external C# / WPF manager

The release avoids relying on old fixed GameAssembly RVAs or machine-specific paths where runtime metadata/reflection and module-relative path resolution are available.

At runtime, the game root and data root are derived from the loaded `<game root>/plugin/sbm.dll` module path. Runtime file access therefore does not depend on the process working directory or a developer-specific drive letter.

### Thread and callback boundaries

The runtime deliberately separates responsibilities:

```text
service worker
  -> config/developer-command polling
  -> JSON parsing and validation
  -> immutable snapshot construction
  -> status/ACK and diagnostic arming

Unity main-thread hooks
  -> character/Animator observation
  -> Transform reads and writes
  -> per-frame pure motion math
```

Animation callbacks contain no configuration file I/O, JSON parsing, sleeps, unbounded logging, or managed allocation.

---

## Motion Modes

### Original

Stops custom transform writes for the selected character and leaves the game to use its native behavior.

The global plugin switch is a stronger no-write gate covering all characters and modes. Character observation continues while globally disabled so switching characters does not leave stale identity state when the plugin is re-enabled.

### Synthetic

Adds a configurable gait-driven oscillator and optional event-driven Jump/Inertia response on top of the current native bone orientation.

Locomotion controls include:

- Walk / Run / Sprint / Zipline Up amplitude
- Walk / Run / Sprint / Zipline Down amplitude
- Walk / Run / Sprint / Zipline frequency
- amplitude attack smoothing
- frequency smoothing
- return-to-idle release smoothing
- character axis and sign

`Idle` remains a native/no-write state in normal product tuning. Unsupported clips may let internal envelopes decay, but they do not compose, write, or replay a synthetic target.

### Amplify Native

Amplifies motion already produced by the game through quaternion-delta exponentiation.

This mode is useful when a character's native motion pattern is already desirable and only needs a stronger response. It uses the verified LateTick timing path and does not share the synthetic oscillator state.

---

## Jump / Inertia System

Jump is a separate source inside Synthetic mode, not a second Transform writer. It replaces the synthetic angle only while a validated active-jump epoch owns motion intent, then uses the same `ComposeTargets -> SafeSetLocalRotation -> absolute-target replay` chain as locomotion.

### Signal contract

The 20 Hz signal layer publishes:

```text
entity / sample serial / sample time
identity validity
clip-read validity
fallingSpeed validity and value
teleport flag
jump_start active
landing active
```

The per-frame source rejects stale, non-finite, teleported, identity-mismatched, or incomplete samples. Character changes reset all jump energy and event state.

### State machine

```text
NativeOnly
    |
    | exact jump_start edge
    v
Armed
    |
    | positive fallingSpeed
    v
Rising
    |
    | fallingSpeed approaches zero
    v
Apex
    |
    | fallingSpeed becomes negative
    v
Falling
    |
    | accepted landing edge
    v
LandingTail
    |
    | configured tail + bounded settle
    v
NativeOnly
```

Important behavioral rules:

- Only an exact active `jump_start` edge arms a new epoch.
- Takeoff begins with a short delayed downward response.
- The response reverses around the apex and stays upward during falling.
- A landing edge injects an impact-dependent angular velocity.
- A direct fall with no active jump epoch remains native-only.
- A stale/invalid signal, teleport, character switch, invalid tuning, or disabled Jump resets fail closed.

### Phase-guided second-order response

The main inertial response follows a damped second-order system:

```text
angle'' + 2*zeta*omega*angle' + omega^2*(angle - target) = external impulse
omega = 2*pi*natural_frequency_hz
zeta = damping_ratio
```

The phase target is:

```text
Armed/Rising after delay:
  target = rising_target_deg

Apex/Falling:
  target = apex_falling_target_deg
           - acceleration_response * filtered_falling_acceleration

LandingTail:
  target returns to zero while the injected landing velocity settles
```

The landing impulse is proportional to the magnitude of landing `fallingSpeed`:

```text
angular_velocity -= landing_impulse_gain * abs(fallingSpeed)
```

`natural_frequency_hz` is spring stiffness/response frequency, not the explicit landing-shake carrier frequency. Lower spring frequency can convert the same impulse into a larger angular displacement.

### Explicit landing shake

LandingTail also adds an explicit damped oscillation:

```text
landingShake(t)
  = amplitude_deg
  * exp(-t / damping_tau_sec)
  * sin(2*pi*frequency_hz*t)
```

The final Jump angle is:

```text
Jump angle = second-order inertial angle + explicit landingShake angle
```

The configured shake duration is honored before the normal settled condition may clear the tail. A separate hard safety cap prevents an unbounded epoch.

### Automatic Jump direction

Ground locomotion keeps the profile's normal `axis.sign`. Jump adds an axis-aware visual-direction factor:

```text
Axis X -> +1
Axis Y -> +1
Axis Z -> -1
```

The final relationship is:

```text
final Jump angle
  = raw Jump source
  * profile axis.sign
  * AutomaticJumpDirection(axis)
```

This factor is applied once to the complete Jump source, including Rising, Falling, acceleration response, landing impulse, and landing shake. It does not change Walk / Run / Sprint / Zipline direction.

### Manager Jump UI

Jump/Inertia appears only under **Main -> Advanced** for the actively controlled character.

User-facing controls include:

- Rising Target (stored with a protected negative direction)
- Apex/Falling Target
- Acceleration Response
- Spring Frequency
- Spring Damping Ratio
- Landing Impulse Gain
- Landing Shake Amplitude
- one discrete Landing Shake Profile slider

The slider selects one coupled tuple:

| Profile | Decay Tau | Shake Frequency | Duration |
|---:|---:|---:|---:|
| 1 | 0.30 | 2.50 | 1.60 |
| 2 | 0.55 | 2.00 | 3.00 |
| 3 | 0.50 | 2.50 | 2.80 |
| 4 | 0.57 | 2.25 | 3.11 |
| 5 | 0.70 | 2.50 | 4.40 |

`takeoff_delay_sec` and `acceleration_filter_tau_sec` remain part of the schema but are hidden from normal UI. The three coupled shake fields also remain fully serialized even though the UI exposes them as one profile slider.

The complete serialized Jump block is:

```json
{
  "enabled": true,
  "mode": "landing_damped",
  "amplitude_deg": 23.333,
  "damping_tau_sec": 0.35,
  "frequency_hz": 3.0,
  "max_duration_sec": 1.2,
  "takeoff_delay_sec": 0.08,
  "rising_target_deg": -23.333,
  "apex_falling_target_deg": 23.333,
  "acceleration_response": 0.1667,
  "acceleration_filter_tau_sec": 0.08,
  "natural_frequency_hz": 2.2,
  "damping_ratio": 0.52,
  "landing_impulse_gain": 6.667
}
```

The values above illustrate the schema and are not a universal character preset. The retained mode name `landing_damped` is a compatibility label; in the current Runtime it enables the complete event-driven Jump/Inertia path, not only the earlier landing-only curve.

---

## Configuration and Data Ownership

The project uses layered configuration rather than one monolithic config file:

```text
data/characters.default.json
  technical character database
  -> stable chr_id
  -> display name
  -> bones
  -> axis/sign
  -> animation_rules
  -> complete generic/safe parameter defaults

presets/<name>.json
  user/product parameter layer
  -> enabled and motion mode
  -> per-gait Up/Down/frequency
  -> envelope
  -> tuned per-character Jump table
  -> native amplification factor

runtime/config.json
  global runtime control
  -> enabled
  -> active_preset
  -> revision

runtime/runtime_status.json
  runtime status and applied-revision acknowledgement
```

Load order:

```text
character database
  -> active preset overrides
  -> runtime global state
  -> strict validation
  -> immutable ConfigSnapshot
```

The runtime accepts partial preset overrides, while the Manager normally writes a full character snapshot. Unknown numeric values must be finite; invalid types, unsupported modes/axes, incomplete bone pairs, and invalid final snapshots are rejected rather than partially installed.

### Hot reload and acknowledgement

Manager Apply performs:

```text
write preset in the Manager folder
  -> mirror preset to the game's SecondaryMotion/presets
  -> increment runtime/config.json revision
  -> runtime rebuilds and validates an immutable snapshot
  -> atomic snapshot swap
  -> runtime_status.applied_revision acknowledgement
```

The Manager polls for acknowledgement. If the game is not running, the data remains saved and applies when the runtime starts later. A failed runtime rebuild retains the last-known-good snapshot.

The service worker checks runtime configuration at a low fixed cadence (currently 250 ms). On Manager startup, if an existing game-side DB or preset is newer than the Manager-side working copy, the newer game file is copied back first so ordinary startup does not silently replace newer runtime data with an older bundled copy.

### v3 Jump-table migration

Release packages preserve existing user data by shipping defaults as `*.template.json` and only seeding missing working files. That protection alone cannot add a new parameter family to old installations, so Manager 3.0 includes a one-time field-level Jump migration.

On first v3 startup:

1. The technical DB receives the complete generic/safe Jump schema for official characters.
2. Every existing valid preset receives the tuned official Jump object from `Default.template.json`.
3. If an official character is absent from an old preset, a minimal override containing only `jump` is added.
4. Existing gait values, bones, axis/sign, names, envelope, native factor, and custom characters are preserved.
5. Original files are backed up as `*.pre_jump_v3`.
6. `data/.jump_defaults_v3` marks the migration complete so later user Jump edits are not overwritten again.

This separates technical DB defaults from the product's per-character tuned Jump table while allowing old users to receive the new parameters without a full preset overwrite.

---

## Character Profiles

A technical character profile can contain:

- canonical normalized character ID
- localized display name
- verified left/right bone names
- fallback-candidate permission
- rotation axis and sign
- special locomotion `animation_rules`
- complete generic defaults for gait, envelope, Jump, and native amplification

The current repository contains 21 official character profiles, including Si (`chr_0036_jsspsi`). This is a data count, not a runtime limit. Updates add missing official profiles from the packaged templates while preserving existing user profiles.

Character IDs remain stable internal keys. Display names may be localized, but localized names are never used as JSON keys or runtime identity.

Visual amplitude is controlled directly by per-character, per-gait Up/Down values. The current implementation has no generic `bone_scale` or `amplitude_scale` multiplier.

---

## Developer Mode

Developer Mode exists mainly for maintaining the supported-character database.

Current workflow:

```text
Detect active character
        ->
Scan skeleton
        ->
Filter and choose verified left/right bones
        ->
Enter display name
        ->
Save character profile
        ->
Runtime revision hot reload
        ->
Tune axis/sign and parameters in Main/Characters
```

Bone scan is a one-shot main-thread diagnostic command. Unknown characters may still be scanned because diagnostics that only require the Animator/root are dispatched before the normal supported-profile write gate.

Axis Test remains an optional temporary diagnostic. It is not required for Save and does not automatically persist an axis result.

New character defaults are copied from the current Aurora model in the Manager's merged data model, while the scanned character's own bones remain independent. Re-saving an existing character preserves its existing parameters unless explicitly edited.

Normal users do not need Developer Mode.

---

## Manager

The external Manager provides four main pages:

- **Main** — current character, global switch, mode, gait controls, and Advanced Jump/Inertia
- **Characters** — browse/edit supported profiles and normal per-character parameters
- **Presets** — switch, duplicate, rename, import/export, and manage parameter sets
- **Developer** — detect, scan, save, axis-test, and validate character support

The Manager is a single C# / WPF codebase. English and Simplified Chinese use parallel `ResourceDictionary` files; release packages set their default language through `default_lang.txt` rather than maintaining a separate Chinese executable or source tree.

Working presets and character data are stored beside the Manager as the movable primary copy. The native Runtime reads only the game's `SecondaryMotion` directory: normal Apply mirrors the active preset, and Developer Save mirrors its character-DB update. Isolated maintenance operations are not described as instant runtime updates unless they perform that mirror/revision step.

The UI is deliberately separated from the injected motion runtime. UI layout, localization, effect-oriented hints, and coupled parameter controls can change without rewriting hooks or motion math.

---

## Installation and Release Layout

Installed runtime layout:

```text
<game root>/
├─ d3dcompiler_47.dll
├─ vulkan-1.dll
├─ plugin/
│  └─ sbm.dll
└─ SecondaryMotion/
   ├─ data/characters.default.json
   ├─ presets/<name>.json
   ├─ runtime/config.json
   ├─ runtime/runtime_status.json
   ├─ developer/
   └─ logs/
```

The proxy loaders at the game root load native plugin DLLs from `plugin/`. A stale legacy `eiem.dll` is removed to prevent double injection.

Community releases are self-contained `win-x64` packages. Normal users do not need the .NET SDK or a separate .NET Desktop Runtime installation. The ZIP contains a top-level `SecondaryMotion/` folder and is named:

```text
ShakingBreastManager-<version>-EN-win-x64.zip
ShakingBreastManager-<version>-ZH-win-x64.zip
```

On first run, the Manager asks for the game root, creates the game-side `SecondaryMotion` working directories, initializes missing data from templates, installs `plugin/sbm.dll`, and installs both proxy loaders at the game root. EN/ZH packages use the same executable; ZH additionally localizes the packaged character-name template and ships the Chinese user guide.

The release pipeline:

```text
build runtime and proxy DLLs
  -> run verification suite
  -> self-contained Manager publish
  -> assemble EN package
  -> assemble ZH package
  -> include templates, guides, runtime DLL, and proxy loaders
  -> generate ZIPs and SHA-256 hashes
```

Manager-local `settings.json`, logs, PDBs, source files, and machine-specific paths are excluded from release packages.

---

## Safety / Fail-Closed Behavior

The runtime favors **no write** over guessing.

Custom writes are disabled when, for example:

- the global plugin switch is off
- the character is unsupported or disabled
- the active profile is invalid
- required bones cannot be resolved as a complete pair
- the current clip is not accepted locomotion
- an active Jump epoch was not authorized by exact start evidence
- Jump evidence is stale, incomplete, non-finite, teleported, or belongs to another entity
- required runtime symbols/hooks are unavailable
- a configuration update fails strict parsing or final validation
- the active character is switching

The runtime does not intentionally reuse a previous character's target, Jump energy, or cached replay target for a new character.

Numerical and lifecycle guards include finite checks, bounded frame delta, bounded integration substeps, signal staleness limits, identity/teleport invalidation, quaternion-safe composition, a verified replay window, and an absolute Jump-tail limit. These safety gates are not exposed as normal user settings.

---

## Diagnostics and Verification

Primary diagnostic outputs include:

```text
<game root>/plugin/sbm_log.txt
<game root>/plugin/breast_probe_log.txt
<game root>/SecondaryMotion/runtime/runtime_status.json
<Manager>/manager_startup.log
<Manager>/logs/manager_changes.log
<Manager>/manager_crash.log
```

The current verification suite covers, among other areas:

- character DB and preset loading
- strict configuration validation
- gait whitelist and per-character animation rules
- asymmetric Up/Down envelopes
- quaternion math and replay gates
- revision hot reload
- runtime path derivation
- developer-command and bone-dump handshakes
- Jump signal/FSM behavior
- delayed Rising, Apex/Falling response, landing impulse, and landing-tail duration
- direct-fall native-only behavior
- automatic Y/Z Jump direction
- Manager UI/schema/serialization wiring

The Runtime regression suite passes **427 tests with 0 failures**. The release checks also run the Manager official-character migration tests, UI/serialization checks, and a complete Runtime build.

---

## Current Scope / Limitations

- Windows x64
- Focused on the actively controlled character
- Background party-member visuals are not a product requirement
- Active Jump/Inertia requires accepted `jump_start`, landing clips, and valid `fallingSpeed`
- Direct falls without an active-jump epoch intentionally remain native
- Unsupported animation clips remain native unless explicitly whitelisted
- New game versions may require a runtime compatibility update
- Unsupported characters remain native until a verified profile is added
- Visual tuning remains character- and model-dependent
- Source-space angles are not a one-to-one measurement of visible mesh deformation

---

## Technical Stack

### Runtime

```text
C++
IL2CPP reflection / metadata and raw-field resolution
MinHook
Unity Transform access
quaternion composition
allocation-free callback-side motion math
JSON configuration snapshots
```

### Manager

```text
C#
.NET 8
WPF / XAML
WPF-UI controls/navigation
bilingual ResourceDictionary localization
self-contained win-x64 publishing
```

The current release version is `3.2.0` for both Manager and Runtime DLL metadata. `publish_release.bat` uses this project version for package names by default.

---

## Research Methodology

The project was developed through many small runtime experiments rather than one large rewrite.

Investigation tools and methods included:

- XXMI / EFMI model inspection
- Blender mesh/bone visualization
- IL2CPP dumps and metadata inspection
- runtime reflection and raw-field validation
- animation clip enumeration and classifier probes
- same-object / same-rig animation tests
- transform recording
- hook call-rate and Unity-frame diagnostics
- quaternion read/write verification
- controlled gait oscillators
- native-motion amplification experiments
- no-write jump signal timelines
- offline Jump FSM replay
- live phase and landing-tail validation

A recurring rule was:

> One experiment should answer one question and have an explicit PASS / FAIL criterion.

That was especially important because multiple early hypotheses were later rejected by stronger runtime evidence. The current Jump system followed the same signal-before-motion process: first establish trustworthy event and movement evidence, then build the bounded motion source, then connect it to the already verified Transform write chain.

---

## Credits / Related Work

The project builds on and learns from the Endfield modding and reverse-engineering ecosystem, including:

- XXMI / EFMI
- MinHook
- Better-Endfield
- EIEM
- EF-Start-Change
- community IL2CPP tooling and dumps

Please keep upstream licenses and credits intact when redistributing derived code.

---

## Disclaimer

This is an unofficial community project intended for experimentation and modding.

Use it at your own risk. Game updates may change runtime structures or behavior and can temporarily break compatibility.
