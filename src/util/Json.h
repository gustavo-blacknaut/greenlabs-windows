#pragma once

// JSON mínimo, só o que o protocolo de sinalização usa.
//
// Escrever isto à mão mantém o projeto sem dependência externa — a mesma
// escolha do servidor em Go. São umas trezentas linhas contra um gerenciador de
// pacotes inteiro, e o formato aqui é conhecido e fechado.
//
// Cobre o que o protocolo pede: objetos, listas, textos com escape (o SDP vem
// cheio de quebras de linha), números, booleanos e nulo. Não cobre o que não
// aparece: notação científica exótica e pares substitutos de UTF-16 fora do
// básico.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gl {

class Json {
public:
    enum class Tipo { Nulo, Booleano, Numero, Texto, Lista, Objeto };

    Json() = default;
    Json(bool v) : tipo_(Tipo::Booleano), booleano_(v) {}
    Json(double v) : tipo_(Tipo::Numero), numero_(v) {}
    Json(int64_t v) : tipo_(Tipo::Numero), numero_(static_cast<double>(v)) {}
    Json(int v) : tipo_(Tipo::Numero), numero_(static_cast<double>(v)) {}
    Json(std::string v) : tipo_(Tipo::Texto), texto_(std::move(v)) {}
    Json(const char* v) : tipo_(Tipo::Texto), texto_(v ? v : "") {}

    static Json objeto() {
        Json j;
        j.tipo_ = Tipo::Objeto;
        return j;
    }
    static Json lista() {
        Json j;
        j.tipo_ = Tipo::Lista;
        return j;
    }

    Tipo tipo() const { return tipo_; }
    bool ehNulo() const { return tipo_ == Tipo::Nulo; }
    bool ehObjeto() const { return tipo_ == Tipo::Objeto; }
    bool ehLista() const { return tipo_ == Tipo::Lista; }

    // Acesso tolerante: campo ausente devolve o padrão em vez de estourar.
    // Sinalização vinda da rede não merece confiança nenhuma.
    std::string texto(const std::string& chave, const std::string& padrao = "") const;
    double numero(const std::string& chave, double padrao = 0) const;
    bool booleano(const std::string& chave, bool padrao = false) const;
    const Json& filho(const std::string& chave) const;
    bool tem(const std::string& chave) const;

    const std::string& comoTexto() const { return texto_; }
    double comoNumero() const { return numero_; }
    bool comoBooleano() const { return booleano_; }
    const std::vector<Json>& itens() const { return lista_; }
    const std::map<std::string, Json>& campos() const { return objeto_; }

    Json& operator[](const std::string& chave) {
        tipo_ = Tipo::Objeto;
        return objeto_[chave];
    }
    void adicionar(Json valor) {
        tipo_ = Tipo::Lista;
        lista_.push_back(std::move(valor));
    }

    std::string paraTexto() const;

    // Devolve nulo quando o texto não é JSON válido.
    static Json analisar(std::string_view bruto);

private:
    static const Json& vazio();
    static void escapar(const std::string& texto, std::string& saida);

    Tipo tipo_ = Tipo::Nulo;
    bool booleano_ = false;
    double numero_ = 0;
    std::string texto_;
    std::vector<Json> lista_;
    std::map<std::string, Json> objeto_;
};

}  // namespace gl
