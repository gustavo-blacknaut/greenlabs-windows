cmake_minimum_required(VERSION 3.25)

project(GreenLabs
    VERSION 0.0.1
    DESCRIPTION "Cliente nativo do GreenLabs Live Streaming"
    LANGUAGES C CXX)

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

# ---------------------------------------------------------- midia (WebRTC)
#
# libdatachannel entra so pelo que nao da para escrever a mao: ICE, DTLS e
# SRTP. DTLS sozinho significaria implementar TLS inteiro.
#
# Vem por FetchContent em vez de vcpkg para nao arrastar um gerenciador de
# pacotes junto, e com mbedTLS no lugar do OpenSSL - e a diferenca entre uns
# 5 MB e uns 1 MB no executavel final.
#
# Este bloco vem ANTES das nossas flags de proposito: /W4 aplicado globalmente
# vazava para as dependencias, e o mbedTLS trata aviso como erro. As nossas
# flags agora sao por alvo, mais abaixo.
include(FetchContent)

# O mbedTLS vem com DTLS-SRTP desligado, e o libdatachannel precisa dele: e a
# extensao que negocia as chaves do SRTP durante o handshake DTLS. Ligar por
# MBEDTLS_USER_CONFIG_FILE evita editar o codigo da dependencia.
#
# Global de proposito: tanto o mbedTLS quanto o libdatachannel precisam ver a
# mesma configuracao, senao um compila com a estrutura e o outro sem.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_compile_definitions("MBEDTLS_USER_CONFIG_FILE=\"greenlabs-mbedtls.h\"")

set(NO_WEBSOCKET      ON  CACHE BOOL "" FORCE)  # temos o nosso
set(NO_EXAMPLES       ON  CACHE BOOL "" FORCE)
set(NO_TESTS          ON  CACHE BOOL "" FORCE)
set(USE_MBEDTLS       ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(ENABLE_PROGRAMS        OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING         OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG        v3.6.2
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(mbedtls)

# O libsrtp usa find_package(MbedTLS), que procura biblioteca instalada no
# sistema e nao enxerga o subprojeto que o FetchContent acabou de trazer.
# Apontar as variaveis para os alvos resolve: o CMake resolve nome de alvo em
# target_link_libraries do mesmo jeito que resolveria um caminho de arquivo.
# Precisa ser caminho absoluto: com nome solto o FindMbedTLS cria uma
# importacao por IMPORTED_LIBNAME, que vira o literal "mbedtls.lib" na linha do
# linkeditor - sem arquivo e, pior, sem dependencia de ordem, entao o Ninja
# tentava linkar antes de o mbedTLS existir.
set(_mbed_dir "${mbedtls_BINARY_DIR}/library")
set(_mbed_pre "${CMAKE_STATIC_LIBRARY_PREFIX}")
set(_mbed_suf "${CMAKE_STATIC_LIBRARY_SUFFIX}")

set(MbedTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(MbedTLS_LIBRARY    "${_mbed_dir}/${_mbed_pre}mbedtls${_mbed_suf}"    CACHE FILEPATH "" FORCE)
set(MbedCrypto_LIBRARY "${_mbed_dir}/${_mbed_pre}mbedcrypto${_mbed_suf}" CACHE FILEPATH "" FORCE)
set(MbedX509_LIBRARY   "${_mbed_dir}/${_mbed_pre}mbedx509${_mbed_suf}"   CACHE FILEPATH "" FORCE)
set(MbedTLS_VERSION    "3.6.2" CACHE STRING "" FORCE)
set(MbedTLS_FOUND      TRUE     CACHE BOOL   "" FORCE)

FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        v0.24.5
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(libdatachannel)

# O caminho absoluto diz ONDE a biblioteca esta, mas nao diz que ela precisa
# ser construida antes. Estas dependencias dizem.
foreach(_alvo datachannel datachannel-static srtp2)
    if(TARGET ${_alvo})
        add_dependencies(${_alvo} mbedtls mbedx509 mbedcrypto)
    endif()
endforeach()

# ------------------------------------------------------------------ nucleo
#
# Nao depende de UI: da para medir e testar sozinho, e e o que o
# greenlabs-probe e o greenlabs-sinal fazem.
add_library(greenlabs_core STATIC
    src/util/Log.cpp
    src/util/Json.cpp
    src/capture/ProcessTree.cpp
    src/capture/AudioCapture.cpp
    src/capture/ScreenCapture.cpp
    src/video/ColorConverter.cpp
    src/encoder/VideoEncoder.cpp
    src/network/WebSocketClient.cpp
    src/network/Signaling.cpp
    src/network/Midia.cpp
    src/config/Config.cpp
    ui/Renderizador.cpp
    ui/Aplicacao.cpp
)

target_include_directories(greenlabs_core PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(greenlabs_core PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX                # senao windows.h define min/max como macro
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS # fopen: a alternativa segura nao e portavel
)

target_link_libraries(greenlabs_core PUBLIC
    LibDataChannel::LibDataChannelStatic  # ICE, DTLS, SRTP e empacotamento RTP
    d3d11         # dispositivo grafico da duplicacao
    dxgi          # Desktop Duplication
    mmdevapi      # ActivateAudioInterfaceAsync
    avrt          # MMCSS, prioridade de tempo real na thread de audio
    ole32         # COM
    runtimeobject # WRL: FtmBase do handler de ativacao
    mfplat        # Media Foundation: encoder H.264 por hardware
    mfuuid        # GUIDs de midia
    strmiids      # GUIDs do ICodecAPI
    ws2_32        # sockets do cliente WebSocket
    crypt32       # base64 do handshake
    bcrypt        # bytes aleatorios da mascara
    d2d1          # interface: formas e imagem
    dwrite        # interface: texto
    shell32       # CommandLineToArgvW
    windowscodecs # WIC: decodifica o PNG da logo
)

# ------------------------------------------------------------ nossos alvos
#
# Aviso severo e otimizacao ficam nos nossos alvos. Aplicar globalmente
# quebrava as dependencias, que tem regras proprias.
function(greenlabs_flags alvo)
    if(MSVC)
        target_compile_options(${alvo} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Gy>   # funcoes em secoes proprias, para o /OPT:REF podar
        )
        target_link_options(${alvo} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF>   # tira o que nao e chamado
            $<$<CONFIG:Release>:/OPT:ICF>   # funde funcoes identicas
        )
    endif()
endfunction()

greenlabs_flags(greenlabs_core)

# -------------------------------------------------------------- aplicativo
#
# WIN32 e o que faz o executavel abrir sem console atras da janela.
add_executable(GreenLabs WIN32 src/main/main.cpp ui/recursos.rc)
target_link_libraries(GreenLabs PRIVATE greenlabs_core)
greenlabs_flags(GreenLabs)

# ------------------------------------------------------------- ferramentas
add_executable(greenlabs-probe tools/probe/main.cpp)
target_link_libraries(greenlabs-probe PRIVATE greenlabs_core)
greenlabs_flags(greenlabs-probe)

add_executable(greenlabs-sinal tools/sinal/main.cpp)
target_link_libraries(greenlabs-sinal PRIVATE greenlabs_core)
greenlabs_flags(greenlabs-sinal)

set_target_properties(GreenLabs greenlabs-probe greenlabs-sinal PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
