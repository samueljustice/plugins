# Audio Plugins Repository

This repository contains audio plugin projects and their dependencies.

## Projects

### Pitch Flattener
A VST3/AU audio plugin that flattens pitch variations in real-time. Kinda decent for removing sweeping Doppler effects, stabilizing warbling tones, or creating unique pitch-locked effects.

- Location: `/pitchflattener/`
- [Project README](pitchflattener/README.md)
- [Latest Release](https://github.com/samueljustice/plugins/releases/tag/pitchflattener-v1.2.5)

### Reversinator
A VST3/AU real-time reverser: a faithful, modernised recreation of Backwards Machine's three effects (Reverse Playback, Forward Backwards and Reverse Repeat), with a live display of the read heads sweeping backwards through your audio.

- Location: `/reversinator/`
- [Project README](reversinator/README.md)
- [Latest Release](https://github.com/samueljustice/plugins/releases/tag/reversinator-v2.0.0)

### Subbertone
A VST3/AU subharmonic generator. Tracks the pitch of your bass and adds a clean sub one octave below, with six saturation types and a resonant, envelope-swept filter for anything from pure sub reinforcement to a growling octave-down bass.

- Location: `/subbertone/`
- [Project README](subbertone/README.md)
- [Latest Release](https://github.com/samueljustice/plugins/releases/tag/subbertone-v1.1.0)

## Dependencies

### SDK
- **JUCE**: Cross-platform C++ application framework for audio applications
- **World**: High-quality speech analysis/synthesis vocoder system
- **RubberBand**: High-quality audio time-stretching and pitch-shifting library

## Building

Each project has its own build instructions. Please refer to the individual project README files for specific build requirements and instructions.

## License

Each project and dependency has its own license. Please refer to the respective LICENSE files in each directory.

## Developer

Created by Samuel Justice  
Website: [www.sweetjusticesound.com](https://www.sweetjusticesound.com)
