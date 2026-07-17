#pragma once
#include <string>
#include <vector>
#include <optional>

// ---------------------------------------------------------------------------
// wasapi_identity.h
//
// PortAudio's public cross-platform API does not expose a persistent,
// unique hardware identifier for a device -- only a display-name string
// (see audio_handler.h's AudioDeviceInfo::name) and a session-local index
// that isn't guaranteed stable across device topology changes. This was
// confirmed against PortAudio's own maintainers' guidance: there is no
// forced/stable device mapping available through the public API, and two
// identical-model devices can be genuinely indistinguishable by name.
//
// Internally, PortAudio's WASAPI backend *does* have access to a real
// persistent ID (Windows' IMMDevice endpoint ID) -- it's just not surfaced
// through PortAudio's public functions. Rather than patch PortAudio itself
// (which would mean maintaining a fork), this module queries Windows'
// WASAPI device enumeration directly, in parallel with PortAudio, and
// correlates the two by matching device name + data-flow direction. The
// result is a real, stable identity string per device (IMMDevice::GetId(),
// which is a persistent device interface path that -- for USB audio
// devices -- embeds the actual USB VID/PID/instance, not just a friendly
// name) that survives device reordering, multiple identical-model devices
// being plugged in simultaneously, and reboots.
//
// This deliberately doesn't replace PortAudio for actual audio I/O --
// PortAudio still owns capture/playback (see audio_handler.h). This module
// only answers "which physical device is this, really," used for stable
// identification/display and for correlating a device across app restarts
// (e.g. remembering "use this exact USB mic" in future via saved config,
// rather than "use whatever's named X", which could silently pick a
// different physical unit next time).
// ---------------------------------------------------------------------------

namespace wm {

struct WasapiEndpointIdentity {
    std::wstring endpointId;      // IMMDevice::GetId() -- persistent, stable, unique per physical endpoint
    std::string friendlyName;     // PKEY_Device_FriendlyName, for correlating to PortAudio's device name
    bool isCaptureDevice = false; // true = input/capture endpoint, false = output/render endpoint
};

class WasapiIdentity {
public:
    // Enumerates all active WASAPI audio endpoints (both capture and
    // render) with their persistent endpoint IDs. Initializes/uninitializes
    // COM internally for the duration of the call (CoInitializeEx with
    // COINIT_MULTITHREADED); safe to call from the UI thread periodically,
    // though this does real enumeration work each call -- see gui.cpp for
    // caching, same reasoning as the ADB device list fix.
    static std::vector<WasapiEndpointIdentity> EnumerateEndpoints();

    // Best-effort correlation: given a PortAudio device's display name and
    // whether it's an input or output device, finds the matching WASAPI
    // endpoint identity by name (PortAudio's WASAPI backend derives its
    // display name from the same friendly-name property, so this is a
    // reliable match in practice, not a loose heuristic -- unlike the
    // libusb-to-PortAudio correlation attempted elsewhere in this project,
    // which has no such guaranteed relationship).
    static std::optional<WasapiEndpointIdentity> FindByName(
        const std::vector<WasapiEndpointIdentity>& endpoints,
        const std::string& portAudioDeviceName,
        bool isCaptureDevice);

    // Short, human-readable fingerprint derived from the endpoint ID (last
    // segment of the device interface path, which for USB audio devices
    // is where the VID/PID/instance-specific portion lives) -- suitable
    // for display next to a device name so two identical-model devices
    // are visibly distinguishable, without showing the full raw ID string.
    static std::string ShortFingerprint(const std::wstring& endpointId);
};

} // namespace wm
