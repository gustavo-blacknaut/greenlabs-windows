#pragma once

// Descoberta do processo a excluir do áudio.
//
// O Discord roda em vários processos (o principal, os renderizadores, o GPU).
// Só um deles é dono da sessão de áudio, e o WASAPI exclui uma *árvore* — então
// é preciso achar quem toca som e subir até a raiz da árvore do Discord.

#include <cstdint>
#include <string>
#include <vector>

namespace gl {

struct ProcessoNomeado {
    uint32_t pid = 0;
    uint32_t ppid = 0;
    std::string nome;  // minúsculo, sem .exe
};

// Uma passada só do Toolhelp32 devolve pid e pid do pai juntos. O capturador
// antigo fazia duas consultas WMI para a mesma informação, e isso custava caro
// o bastante para travar a máquina quando repetido a cada conexão.
std::vector<ProcessoNomeado> listarProcessos();

// PIDs que têm sessão de áudio ativa no dispositivo de saída padrão.
std::vector<uint32_t> pidsComSessaoDeAudio();

// Devolve a raiz da árvore de processos a excluir, ou 0 se nenhum dos nomes
// estiver rodando.
//
// `nomes` são fragmentos em minúsculo: "discord" casa com "Discord",
// "DiscordPTB" e "DiscordCanary".
//
// A subida para na primeira vez que o pai NÃO casa com os nomes. Sem essa
// guarda a subida chega no explorer.exe e a árvore excluída passa a ser a
// máquina inteira — foi exatamente o que aconteceu na versão 0.2.7 do app em
// Electron: o Discord acabava dentro da árvore "própria" e nunca era excluído.
uint32_t acharRaizParaExcluir(const std::vector<std::string>& nomes);

// Nome do processo, ou "?" se ele já morreu. Só para log.
std::string nomeDoProcesso(uint32_t pid);

}  // namespace gl
