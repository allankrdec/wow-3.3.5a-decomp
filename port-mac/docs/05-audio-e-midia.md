# 05: Áudio, voice chat, filmes e periféricos

## FMOD Ex

- **Origem.** `Engine/Source/SoundEngine/fmodex/` é **código-fonte do próprio FMOD Ex**, não decompilação: tem
  comentários originais e `#ifdef` para PS2, Xenon, Wii e iPhone. São 467 arquivos e ~270 mil linhas.
  `SoundEngine/CMakeLists.txt` compila tudo como `fmodex STATIC`, junto com ogg/vorbis, FLAC e reverbs.
- **Plugins de saída presentes** (todos em `fmodex/win32/src/`):
  - DirectSound (+EAX), DirectSound record, WinMM, WASAPI, ASIO e OpenAL;
  - os genéricos `nosound`, `wavwriter`, `emulated`, `software` e `polled`.
- **Não há saída CoreAudio.** Não existe `fmod_output_coreaudio` nem pasta `mac/`. Só restaram os ganchos:
  - `fmod_settings.h:70-84` detecta `PLATFORM_MAC` via `__MACH__`.
  - `fmod_settings.h:691-693` liga `FMOD_SUPPORT_COREAUDIO`.
  - `fmod.h:274` declara `FMOD_OUTPUTTYPE_COREAUDIO`.
- **Apple Silicon.** `fmod_settings.h:81-84` só distingue `PLATFORM_MAC_INTEL` de `PLATFORM_MAC_PPC`, então arm64
  cairia no ramo PPC (big-endian). É preciso acrescentar um ramo para arm64 little-endian.
- **Como a saída é escolhida.**
  - `win32/src/fmod_os_output.cpp:32-97`: o padrão é WASAPI a partir do Vista e DirectSound antes dele.
  - No jogo, `SoundEngine.cpp:3152` usa `setOutput(AUTODETECT)`, e `:3194` força OpenAL se o cvar pedir.
  - Em caso de erro, o fallback é `NOSOUND` (`:3231`, `:3425`, `:3504`, `:3544`).
- **SIMD e assembly.**
  - Os 6 arquivos `*_asm.s` e o ia32 do FLAC só entram no build de 32 bits (`CMakeLists.txt:57`).
  - No x64, `FMOD_SUPPORT_SIMD` está desligado, e são usados os caminhos em C.
  - O bloco Mac também não liga SIMD, então o arm64 fica limpo.

### O que escrever para o Mac

| Item | Descrição | Dias |
|---|---|---:|
| `fmodex/mac/src/fmod_os_misc.cpp` | ~28 funções. Hoje são 885 linhas Win32: memória, arquivo, tempo, threads, CRITICAL_SECTION, semáforo/MeteredSection, LoadLibrary, avrt. No Mac: pthread, `dispatch_semaphore`, `mach_absolute_time` e `dlopen` | 2 a 3 |
| `fmod_os_net.cpp` | 517 linhas de winsock, para streams por rede. Pode ser stub | 0,5 |
| `fmod_os_output.cpp` (Mac) | registra só CoreAudio, nosound e wavwriter | 0,5 |
| `fmod_output_coreaudio.cpp` | AudioUnit (`kAudioUnitSubType_DefaultOutput`/HAL) com callback de render que chama o mixer de software do FMOD. O modelo é `fmod_output_polled.cpp` ou a interface software. Inclui enumeração de dispositivos (`AudioObjectGetPropertyData`) | 3 a 5 |
| Gravação CoreAudio | AUHAL input, para o voice chat e o `recordStart` | 2 a 3 |
| CMake e `#ifdef` | desligar CDDA, ASF/wmsdk, ACM (`fmod_codec_wav.cpp:15,354,760`), winamp e VST; ramo arm64 em `fmod_settings.h`; `ASSERT` com `int 3` → `__builtin_trap` | 1 a 2 |
| Testes | 3D, reverb (I3DL2/SFX), streaming, música, ogg/mp3, troca de dispositivo | 3 a 5 |

**Total do áudio:** ~2,5 a 4 semanas.

**Atalho:** subir o jogo primeiro com `FMOD_OUTPUTTYPE_NOSOUND`, que já é o fallback, e escrever o CoreAudio em
paralelo.

**Alternativa descartada:** a saída OpenAL existente. O OpenAL do macOS está deprecated, o loader
(`LoadOAL.cpp`) espera uma DLL e a saída depende de EAX. Escrever o CoreAudio é mais limpo.

**Também descartado:** usar um FMOD Ex Mac pré-compilado. Não existe para arm64, e `SoundEngine`/`SoundInterface`
dependem das partes internas desta versão.

## Voice chat (ComSat)

- A captura e a reprodução passam pelo FMOD, num segundo `FMOD::System` (`ComSatSoundIOSoundEngine.cpp`,
  `SoundEngine.cpp:1348 recordStart`). No Mac, a captura vem do plugin CoreAudio, e o `Info.plist` precisa de
  `NSMicrophoneUsageDescription`.
- O codec SPIRIT (`ComSat/SPIRIT/`) é C++ portável, com 6 usos de SSE (`_mm_rsqrt_ss`, `_mm_cvtss_si32`). Ver doc 07.
- A rede UDP (`ComSat/Win64/ComSatNetworkSocketPosix.cpp`, 68 linhas) usa BSD sockets mais `ioctlsocket`,
  `closesocket` e `WSAIoctl(SIO_UDP_CONNRESET)`. A conversão é trivial.
- Voice chat é pouco usado em servidores privados e pode ficar para a última fase.

## Filmes (cinemáticas)

- `Engine/Source/Frame/CSimpleMovieFrame.cpp` faz o parse do AVI por conta própria e chama a API `DivxDecoder`
  (`:441`, `:499`, `:522`). O áudio do filme sai pelo FMOD (`:588`).
- No x64, `Tools/XvidDecoder` (117 linhas) implementa essa API sobre o xvidcore. Hoje só vem
  `ThirdParty/xvidcore/lib/x64/libxvidcore.lib`.
- **Mac:**
  1. Compilar o xvidcore a partir do código-fonte. É C portável; em arm64 usar `--disable-assembly`.
  2. Gerar `XvidDecoder` como estático, sem precisar de dylib.
- `WowSvcs/WowSvcsClient/ChecksumExecutables.cpp:13` inclui `DivxDecoder.dll` no checksum do executável. Ver
  doc 06.
- **Esforço:** 1 a 2 dias.

## Periféricos

| Componente | Situação | Ação no Mac |
|---|---|---|
| `ThirdParty/lgLcd` | já é stub (33 linhas, todas as funções retornam erro), mas usa tipos Win32 | excluir |
| `WoW/Source/Ui/UILCD/Windows/*` (24 arquivos, GDI) | SDK do teclado LCD da Logitech | excluir. `UILCD/LCD.cpp` vira um stub vazio |
| `Engine/Source/SteelseriesMouse` (HID + SetupAPI, 620 linhas) | criado em `UIUtil/InputControl.cpp:82` (cvar `enableWowMouse`) | stub com `GetDeviceCount() == 0` |
| Joystick (DirectInput8) | `Os/Win64/Joystick.cpp` | stub. Depois, GameController.framework se quiser |
