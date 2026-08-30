#include "config/Config.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "util/Json.h"
#include "util/Log.h"

namespace gl {
namespace {

std::string pastaDoAplicativo() {
    PWSTR bruto = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &bruto))) {
        return {};
    }

    const int n = ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, nullptr, 0, nullptr, nullptr);
    std::string saida(n > 1 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 1) {
        ::WideCharToMultiByte(CP_UTF8, 0, bruto, -1, saida.data(), n, nullptr, nullptr);
    }
    ::CoTaskMemFree(bruto);

    if (saida.empty()) return {};
    saida += "\\GreenLabs";

    // Cria a pasta se ainda não existe. Erro de "já existe" não é erro.
    const int largura = ::MultiByteToWideChar(CP_UTF8, 0, saida.c_str(), -1, nullptr, 0);
    std::wstring wide(largura > 1 ? static_cast<size_t>(largura - 1) : 0, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, saida.c_str(), -1, wide.data(), largura);
    ::CreateDirectoryW(wide.c_str(), nullptr);

    return saida;
}

}  // namespace

std::string Config::caminho() {
    const std::string pasta = pastaDoAplicativo();
    if (pasta.empty()) return {};
    return pasta + "\\config.json";
}

Config Config::carregar() {
    Config padrao;

    const std::string arquivo = caminho();
    if (arquivo.empty()) return padrao;

    std::ifstream entrada(arquivo, std::ios::binary);
    if (!entrada) return padrao;

    std::ostringstream buffer;
    buffer << entrada.rdbuf();

    const Json json = Json::analisar(buffer.str());
    if (!json.ehObjeto()) {
        aviso("config.json ilegivel; usando os padroes");
        return padrao;
    }

    Config c;
    c.servidor = json.texto("servidor");
    c.sala = json.texto("sala", padrao.sala);
    c.nome = json.texto("nome");
    c.qualidade = static_cast<int>(json.numero("qualidade", padrao.qualidade));
    c.monitor = static_cast<int>(json.numero("monitor", padrao.monitor));
    c.audio = json.booleano("audio", padrao.audio);
    c.volume = static_cast<int>(json.numero("volume", padrao.volume));
    for (const Json& item : json.filho("telas").itens()) {
        if (item.tipo() == Json::Tipo::Numero) c.telas.push_back(static_cast<int>(item.comoNumero()));
    }
    for (const Json& item : json.filho("cameras").itens()) {
        if (item.tipo() == Json::Tipo::Texto && !item.comoTexto().empty()) {
            c.cameras.push_back(item.comoTexto());
        }
    }

    for (const Json& item : json.filho("servidores").itens()) {
        if (item.tipo() == Json::Tipo::Texto && !item.comoTexto().empty()) {
            c.servidores.push_back(item.comoTexto());
        }
    }
    return c;
}

void Config::lembrarServidor(const std::string& endereco) {
    if (endereco.empty()) return;
    std::erase(servidores, endereco);
    servidores.insert(servidores.begin(), endereco);
    if (servidores.size() > 6) servidores.resize(6);
}

void Config::salvar() const {
    const std::string arquivo = caminho();
    if (arquivo.empty()) return;

    Json json = Json::objeto();
    json["servidor"] = Json{servidor};
    json["sala"] = Json{sala};
    json["nome"] = Json{nome};
    json["qualidade"] = Json{qualidade};
    json["monitor"] = Json{monitor};
    json["audio"] = Json{audio};
    json["volume"] = Json{volume};
    Json listaTelas = Json::lista();
    for (int t : telas) listaTelas.adicionar(Json{t});
    json["telas"] = listaTelas;

    Json listaCameras = Json::lista();
    for (const auto& c : cameras) listaCameras.adicionar(Json{c});
    json["cameras"] = listaCameras;

    Json lista = Json::lista();
    for (const auto& s : servidores) lista.adicionar(Json{s});
    json["servidores"] = lista;

    // Escreve num temporário e renomeia por cima. Se o aplicativo for fechado
    // no meio da escrita, o arquivo antigo continua inteiro em vez de virar
    // metade de um JSON.
    const std::string temporario = arquivo + ".novo";
    {
        std::ofstream saida(temporario, std::ios::binary | std::ios::trunc);
        if (!saida) return;
        saida << json.paraTexto();
    }
    ::MoveFileExA(temporario.c_str(), arquivo.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace gl
