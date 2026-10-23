# 04: Gráficos (Gx / Gxu / ShaderEffect)

## Estado atual

Os três subdiretórios somam ~31,9 mil linhas.

| Backend | Arquivos | Linhas | Estado |
|---|---|---:|---|
| D3D9 | `CGxDeviceD3d/CGxDeviceD3d.cpp` + `.h`, `CGxD3dDevice.cpp` | ~4.500 | padrão (`GxDefaultApi()` → `GxApi_D3d9`, `Gx.cpp:117-119`) |
| D3D9Ex | `CGxDeviceD3d9Ex/*` | ~4.800 | alternativo |
| **OpenGL** | `CGxDeviceOpenGL/CGxDeviceOpenGl.cpp` (5.213), `GlExt.cpp`/`.h` (~950), `CGxVboBroker1`, `Types.h` | **~6.580** | **completo**: 115 funções com `OFFSET`, todo método do `.h` implementado |
| Base comum | `CGxDevice/CGxDevice.cpp` + `.h` | ~2.470 | portável |
| Plataforma | `CGxDevice/Win64/CGxDevice.cpp` | 596 | Win32: enumeração de modos e adaptadores |

Como o backend é escolhido:

- O cvar `gxApi` e as opções de linha de comando `-opengl`/`-d3d`/`-d3d9ex`
  (`WoW/Source/ConsoleDevice/ConsoleDevice.cpp:378-402`) decidem o device.
- A fábrica é `CreateGxDevice` (`Gx.cpp:179-209`).
- O enum também prevê `GxApi_GLL`, a camada GL do cliente Mac original, que nunca foi implementada aqui.

O D3DX não é usado. O nvapi só aparece no D3D, para estéreo 3D.

## O backend OpenGL

- **Tipo de GL:** legado 1.x/2.x com fixed-function: `glTexEnvi`, `glLightfv`, `glMaterialfv`, `glTexGen`,
  `glFog` e pilha de matrizes.
- **Shaders:** `CGxDevice::IShaderLoad` (`CGxDevice.cpp:1673-1750`) lê `Shaders\{Vertex|Pixel}\<perfil>\<nome>.bls`
  das MPQs. O magic é `GXSH`, versão `0x10003`, com N permutações.
  - No OpenGL, o blob é **texto ARB assembly**, carregado com `glProgramStringARB` para `arbvp1`/`arbfp1`
    (`CGxDeviceOpenGl.cpp:4609`, `4644`).
  - Prioridade (`ISetCaps`, `CGxDeviceOpenGl.cpp:733-764`): pixel `arbfp1` > `nvts*` > `nvrc`; vertex `arbvp1` >
    `arbvp1_cg12` > `nvvp*`.
  - Os caminhos NV/ATI simplesmente não são escolhidos quando a extensão não existe.
- **Extensões detectadas** (`GlExt.cpp:405-643`): ARB multitexture, texture_compression, VBO,
  texture_env_combine, vertex_program, fragment_program, cube_map, multisample, occlusion_query,
  point_sprite e texture_rectangle; EXT bgra, s3tc, anisotropic, lod_bias, framebuffer_object e blend_color;
  SGIS_generate_mipmap. Também WGL_ARB_pbuffer, WGL_EXT_swap_control e WGL_ARB_pixel_format.
- **Código Win32/WGL:** ~1.300 linhas, cerca de 25% do backend.

  | Trecho | Linhas em `CGxDeviceOpenGl.cpp` |
  |---|---|
  | janela e contexto dummy | 1414-1452 |
  | pbuffer | 1468-1755 |
  | fullscreen e modos de vídeo (`ChangeDisplaySettingsExA`) | 1490-1540 |
  | gamma (`Get/SetDeviceGammaRamp`) | 1560-1630 |
  | criação da janela | 1637-1709 |
  | pixel format | 1840-2065 |
  | WndProc | 2126-2375 |
  | `DeviceCreate`/`RegisterClass` | 2378-2560 |
  | enumeração de formatos | 2772-2830 |
  | `wglSwapLayerBuffers` | 3905 |

  O resto é GL padrão.
- **Ponteiros de função:** todos vêm de `wglGetProcAddress` (73 chamadas, `GlExt.cpp`).

Partes do jogo dependem da API em uso. Elas testam `DeviceApi()==GxApi_OpenGl` para inverter a projeção ou mudar
de caminho: `Gx.cpp:264`, `MapRenderChunk.cpp:1971`, LiquidSystem, Rain, Snow, Sand, DetailDoodad e
`PortraitButton.cpp:76,133`. Como esses caminhos já existem e foram decompilados, **o modo OpenGL é jogável no
Windows hoje** (`Wow64.exe -opengl`). Esse é o primeiro teste a fazer antes do porte (ver doc 08).

## Opções para o macOS

### (a) Reaproveitar o backend OpenGL com contexto legado 2.1 via CGL/NSOpenGL (recomendado para o 1º porte)

O macOS ainda oferece, inclusive no Apple Silicon com o GL implementado sobre Metal, o contexto
`NSOpenGLProfileVersionLegacy` (GL 2.1). Ele tem tudo que o backend usa:

- `GL_ARB_vertex_program` / `GL_ARB_fragment_program`;
- ARB_vbo, EXT_framebuffer_object, S3TC, texture_env_combine, occlusion_query, anisotropic;
- fixed-function completo.

**Os `.bls` arbvp1/arbfp1 já estão nas MPQs do cliente 3.3.5a**, porque o cliente Windows também roda em modo
OpenGL. Nenhum shader precisa ser traduzido.

Trabalho:

1. Criar `CGxDeviceOpenGL/Mac/` (ou `#if`) com:
   - `NSOpenGLContext`/`CGLContextObj` na `NSView` fornecida pelo `Os/Mac`;
   - `CGLSetParameter(kCGLCPSwapInterval)` para o vsync; na prática o GL sobre Metal não respeita o swap interval
     (~80 fps numa tela de 60 Hz), então o `ScenePresent` do Mac espera o próximo refresh contado por um
     `CVDisplayLink` quando o vsync está ligado;
   - `flushBuffer` no present.
2. Trocar pbuffer por FBO, que o backend já sabe usar quando `EXT_framebuffer_object` existe. Verificar se ainda
   há algum caminho que obrigue pbuffer.
3. Resolver ponteiros GL por `dlsym(RTLD_DEFAULT, ...)` ou ligação estática ao `OpenGL.framework`.
4. Gamma: `CGSetDisplayTransferByTable`, ou ignorar gamma de hardware em modo janela, que é o padrão moderno.
5. Modos de vídeo: `CGDisplayCopyAllDisplayModes` só para listar resoluções. O fullscreen deve ser janela sem
   borda.
6. Portar `CGxDevice/Win64/CGxDevice.cpp` (~600 linhas) para `Mac/`: adaptadores, formatos e `AdapterFormats`.
7. Retina e escala: decidir entre pixels físicos e pontos (doc 03).
8. CMake: tirar o `d3d9.lib` sem condição e compilar só o backend GL no Mac. `GxDefaultApi()` passa a retornar
   `GxApi_OpenGl` no Mac.

**Esforço:** 3 a 6 semanas.

**Riscos:**

- O OpenGL está *deprecated* desde o macOS 10.14. Ele funciona no macOS 26, mas pode ser removido.
- O driver GL-sobre-Metal da Apple tem bugs pontuais com ARB programs e com alguns estados fixed-function.
  Espere 1 a 2 semanas de ajustes visuais.

### (b) Backend Metal nativo (recomendado como 2ª etapa)

Um `CGxDeviceMetal` novo, implementando a mesma interface `CGxDevice`:

- **Shaders:** traduzir em tempo de carga o ARB assembly (arbvp1/arbfp1) para MSL. O ARB asm é uma linguagem
  pequena e regular, então um tradutor dedicado fica em ~2-3 mil linhas. A alternativa é traduzir o bytecode
  D3D9 SM2/3 dos `.bls` vs_2_0/ps_2_0 com SPIRV-Cross ou um conversor estilo DXVK. Os resultados podem ir para um
  cache em disco.
- **Fixed-function:** emular em MSL o que o cliente usa: combiners (`ISetColorOp`/`ISetAlphaOp`), luzes, fog,
  texgen, alpha test e a pilha de matrizes. Gerar variantes de shader por estado.
- **Estado estilo D3D9 → Pipeline State Objects:** usar um cache de PSO por hash de estado (blend, depth,
  formato de vértice e shaders).
- **Recursos:** `MTLBuffer` dinâmico com ring buffer, `MTLTexture` com DXT (BC1-3, suportado no Apple Silicon e
  em Macs Intel) e queries de oclusão (`visibilityResultBuffer`).

**Esforço:** ~8 a 12 mil linhas novas, 4 a 8 meses para uma pessoa. É à prova de futuro e rende mais. Não bloqueia
o porte, porque pode substituir (a) depois.

### (c) Camada de tradução D3D9 → Vulkan → MoltenVK (DXVK-native) (não recomendado)

Mantém o backend D3D9 decompilado, mas ele depende de HWND, de COM e de `IDirect3D9`. Seria preciso:

- um shim de janela;
- compilar o dxvk-native para macOS, o que não tem suporte oficial;
- conviver com as limitações do MoltenVK (sem geometry shaders, etc.).

São 1 a 3 meses de integração frágil e dependências pesadas. A única vantagem seria usar exatamente o mesmo
caminho de renderização do Windows.

## Recomendação

Fazer **(a)** primeiro, para chegar rápido a um porte funcional que valida todo o resto (Os, rede, áudio, input).
Fazer **(b)** como projeto paralelo ou posterior, atrás da mesma interface `CGxDevice`, e trocar o padrão para
Metal quando houver paridade visual.

Fontes (Gxu) usam FreeType, que é portável. BLP e texturas (`BLPFile`, `Images`) são C++ puro.
