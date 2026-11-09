# LE/Classic handoff and Samsung profile switches

## Explicit LE disable

The DZIF `LeAudioService.setConnectionPolicy(device, FORBIDDEN)` path stores the policy and then disables and disconnects LE. If an allowed Classic connection is already present, removing LE before making that connection active can temporarily leave no media output and pause a player.

The 0.8.2 candidate prepares an already connected A2DP output before the existing LE-disable sequence. It requires normal media mode, the requested device to belong to the currently active LE output group, A2DP policy to be ALLOWED, and A2DP connection state to be CONNECTED. Only a successful `setActiveDevice` followed by matching readback permits `removeActiveDevice(hasFallbackDevice=true)`. Call/ringtone modes and rejected, unavailable, forbidden, unknown-policy and unrelated targets retain the original disconnect behavior.

The helper changes neither connection permissions nor preferred-profile storage, and sends no media-play command. It does not create a Classic connection, retry indefinitely, suppress real disconnections globally, or promise uninterrupted switching when the target is not connected. The stock manager already inverts its stop-audio flag correctly; that conversion is unchanged.

Host checks execute the actual inserted smali across target/group/service/policy/connection/rejection/readback combinations. The Android build also verifies the helper in the decoded, rebuilt exact-input APK and its ordering before LE disable/disconnect. Phone handoff and listening validation remain required; 0.8.2 was prepared after ADB disconnection and is not the heard 0.8.1 generation.

## Call/Audio switch review

The inspected DZIF Settings code uses `SecBluetoothDetailsProfilesController.refreshProfileSwitchPreference` and its `ExternalSyntheticLambda2` predicate. For ordinary profile switches, it checks whether a cached Classic profile is CONNECTING or CONNECTED. This can display Call/Audio as OFF while the device is playing through LE. The click handler also derives the requested enable operation from connection state in one path.

A display correction therefore needs both rendering and click behavior reviewed together. Changing only the checked appearance could make an OFF tap enable the Classic profile instead. The intended follow-up is to use Android's saved permissions and preferred output/duplex transport for an active LE device, preserve explicit user-disabled permissions, and retain the existing Classic and unrelated-profile behavior.

No Settings APK or UI change is included in this candidate. Settings is not one of the module's five verified stock inputs or three runtime mounts. Its exact installed path, hash, process lifecycle and native toggle behavior need device validation before adding it to the installation surface. No preference-replay watcher has been reintroduced to make the switches appear enabled.

## Current device evidence

The user confirmed LC3 playback on 0.8.1 after an earbud case cycle and reconnection. Its Classic-policy activation guard passed 2,592 APK cases, but a Classic-forbidden LE-only trial also produced CIS/ACL timeouts. The final heard session had A2DP/headset permissions allowed. QHS remained enabled; the earlier 0.8.0 Classic test matched an Air6 ACL to the reference QHS-state1 notification prefix. These observations do not establish a receiver PCM comparison, every switching scenario, or full kernel/DFRoot reboot behavior.
