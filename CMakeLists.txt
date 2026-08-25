cmake_minimum_required(VERSION 3.25)

project(GreenLabs
    VERSION 0.0.1
    DESCRIPTION "Cliente nativo do GreenLabs Live Streaming"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Este cliente e especifico para Windows: usa DXGI e WASAPI.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Nao usamos modulos C++20; sem isto o CMake gera regras de scan a mais.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Sem dependencia do redistributable do Visual C++: o .exe roda em maquina
# limpa, que e o ponto de sair do Electron.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
    UNICODE
    _UNICODE
    NOMINMAX                 # senao windows.h define min/max como macro
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    add_compile_options($<$<CONFIG:Release>:/O2>)
    add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# ---------------------------------------------------------------- nucleo
#
# Nao depende de UI nem de rede: da para medir e testar sozinho, e e o que o
# greenlabs-probe faz.
add_library(greenlabs_core STATIC
    src/util/Log.cpp
    src/capture/ProcessTree.cpp
    src/capture/AudioCapture.cpp
    src/capture/ScreenCapture.cpp
)

target_include_directories(greenlabs_core PUBLIC src)

# Tudo do Windows SDK. Nenhuma dependencia externa nesta etapa.
target_link_libraries(greenlabs_core PUBLIC
    d3d11        # dispositivo grafico da duplicacao
    dxgi         # Desktop Duplication
    mmdevapi     # ActivateAudioInterfaceAsync
    avrt         # MMCSS, prioridade de tempo real na thread de audio
    ole32        # COM
    runtimeobject # WRL: FtmBase do handler de ativacao
)

# ---------------------------------------------------------------- probe
add_executable(greenlabs-probe tools/probe/main.cpp)
target_link_libraries(greenlabs-probe PRIVATE greenlabs_core)

set_target_properties(greenlabs-probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
