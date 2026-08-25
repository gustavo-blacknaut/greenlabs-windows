#include "util/Json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace gl {
namespace {

struct Leitor {
    std::string_view texto;
    size_t pos = 0;
    bool erro = false;

    void pularEspaco() {
        while (pos < texto.size()) {
            const char c = texto[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
            else break;
        }
    }
    char atual() const { return pos < texto.size() ? texto[pos] : '\0'; }
    bool consumir(char c) {
        if (atual() == c) {
            ++pos;
            return true;
        }
        return false;
    }
};

void escreverUtf8(uint32_t ponto, std::string& saida) {
    if (ponto < 0x80) {
        saida.push_back(static_cast<char>(ponto));
    } else if (ponto < 0x800) {
        saida.push_back(static_cast<char>(0xC0 | (ponto >> 6)));
        saida.push_back(static_cast<char>(0x80 | (ponto & 0x3F)));
    } else if (ponto < 0x10000) {
        saida.push_back(static_cast<char>(0xE0 | (ponto >> 12)));
        saida.push_back(static_cast<char>(0x80 | ((ponto >> 6) & 0x3F)));
        saida.push_back(static_cast<char>(0x80 | (ponto & 0x3F)));
    } else {
        saida.push_back(static_cast<char>(0xF0 | (ponto >> 18)));
        saida.push_back(static_cast<char>(0x80 | ((ponto >> 12) & 0x3F)));
        saida.push_back(static_cast<char>(0x80 | ((ponto >> 6) & 0x3F)));
        saida.push_back(static_cast<char>(0x80 | (ponto & 0x3F)));
    }
}

uint32_t lerHex4(Leitor& l) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && l.pos < l.texto.size(); ++i) {
        const char c = l.texto[l.pos++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
        else l.erro = true;
    }
    return v;
}

std::string lerTexto(Leitor& l) {
    std::string saida;
    if (!l.consumir('"')) {
        l.erro = true;
        return saida;
    }
    while (l.pos < l.texto.size()) {
        const char c = l.texto[l.pos++];
        if (c == '"') return saida;
        if (c != '\\') {
            saida.push_back(c);
            continue;
        }
        if (l.pos >= l.texto.size()) break;
        const char escapado = l.texto[l.pos++];
        switch (escapado) {
            case '"':  saida.push_back('"');  break;
            case '\\': saida.push_back('\\'); break;
            case '/':  saida.push_back('/');  break;
            case 'b':  saida.push_back('\b'); break;
            case 'f':  saida.push_back('\f'); break;
            case 'n':  saida.push_back('\n'); break;
            case 'r':  saida.push_back('\r'); break;
            case 't':  saida.push_back('\t'); break;
            case 'u': {
                uint32_t ponto = lerHex4(l);
                // Par substituto: o JSON escapa fora do plano básico em duas
                // metades, e juntá-las é o que faz emoji em apelido sobreviver.
                if (ponto >= 0xD800 && ponto <= 0xDBFF && l.pos + 1 < l.texto.size() &&
                    l.texto[l.pos] == '\\' && l.texto[l.pos + 1] == 'u') {
                    l.pos += 2;
                    const uint32_t baixo = lerHex4(l);
                    if (baixo >= 0xDC00 && baixo <= 0xDFFF) {
                        ponto = 0x10000 + ((ponto - 0xD800) << 10) + (baixo - 0xDC00);
                    }
                }
                escreverUtf8(ponto, saida);
                break;
            }
            default:
                l.erro = true;
                return saida;
        }
    }
    l.erro = true;
    return saida;
}

Json lerValor(Leitor& l, int profundidade);

Json lerObjeto(Leitor& l, int profundidade) {
    Json obj = Json::objeto();
    l.consumir('{');
    l.pularEspaco();
    if (l.consumir('}')) return obj;

    for (;;) {
        l.pularEspaco();
        const std::string chave = lerTexto(l);
        if (l.erro) return obj;
        l.pularEspaco();
        if (!l.consumir(':')) {
            l.erro = true;
            return obj;
        }
        // Chave repetida: a última vence, igual ao JSON.parse do navegador. É o
        // que garante que o "from" carimbado pelo servidor prevaleça sobre um
        // "from" que o cliente tenha mandado.
        obj[chave] = lerValor(l, profundidade + 1);
        if (l.erro) return obj;
        l.pularEspaco();
        if (l.consumir(',')) continue;
        if (l.consumir('}')) return obj;
        l.erro = true;
        return obj;
    }
}

Json lerLista(Leitor& l, int profundidade) {
    Json lista = Json::lista();
    l.consumir('[');
    l.pularEspaco();
    if (l.consumir(']')) return lista;

    for (;;) {
        lista.adicionar(lerValor(l, profundidade + 1));
        if (l.erro) return lista;
        l.pularEspaco();
        if (l.consumir(',')) continue;
        if (l.consumir(']')) return lista;
        l.erro = true;
        return lista;
    }
}

Json lerValor(Leitor& l, int profundidade) {
    // Teto de aninhamento: JSON vindo da rede pode ser feito de propósito para
    // estourar a pilha com colchetes aninhados.
    if (profundidade > 64) {
        l.erro = true;
        return Json{};
    }
    l.pularEspaco();
    const char c = l.atual();

    if (c == '{') return lerObjeto(l, profundidade);
    if (c == '[') return lerLista(l, profundidade);
    if (c == '"') return Json{lerTexto(l)};

    if (l.texto.compare(l.pos, 4, "true") == 0) {
        l.pos += 4;
        return Json{true};
    }
    if (l.texto.compare(l.pos, 5, "false") == 0) {
        l.pos += 5;
        return Json{false};
    }
    if (l.texto.compare(l.pos, 4, "null") == 0) {
        l.pos += 4;
        return Json{};
    }

    const size_t inicio = l.pos;
    if (c == '-' || c == '+') ++l.pos;
    while (l.pos < l.texto.size()) {
        const char d = l.texto[l.pos];
        if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E' || d == '-' || d == '+') {
            ++l.pos;
        } else {
            break;
        }
    }
    if (l.pos == inicio) {
        l.erro = true;
        return Json{};
    }

    double valor = 0;
    const char* comeco = l.texto.data() + inicio;
    const char* fim = l.texto.data() + l.pos;
    if (std::from_chars(comeco, fim, valor).ec != std::errc{}) {
        l.erro = true;
        return Json{};
    }
    return Json{valor};
}

}  // namespace

const Json& Json::vazio() {
    static const Json nada;
    return nada;
}

bool Json::tem(const std::string& chave) const {
    return tipo_ == Tipo::Objeto && objeto_.find(chave) != objeto_.end();
}

const Json& Json::filho(const std::string& chave) const {
    if (tipo_ != Tipo::Objeto) return vazio();
    const auto achou = objeto_.find(chave);
    return achou == objeto_.end() ? vazio() : achou->second;
}

std::string Json::texto(const std::string& chave, const std::string& padrao) const {
    const Json& v = filho(chave);
    return v.tipo_ == Tipo::Texto ? v.texto_ : padrao;
}

double Json::numero(const std::string& chave, double padrao) const {
    const Json& v = filho(chave);
    if (v.tipo_ == Tipo::Numero) return v.numero_;
    // Número que veio como texto ainda é número: alguns clientes mandam assim.
    if (v.tipo_ == Tipo::Texto) {
        double saida = 0;
        const char* comeco = v.texto_.data();
        const char* fim = comeco + v.texto_.size();
        if (std::from_chars(comeco, fim, saida).ec == std::errc{}) return saida;
    }
    return padrao;
}

bool Json::booleano(const std::string& chave, bool padrao) const {
    const Json& v = filho(chave);
    return v.tipo_ == Tipo::Booleano ? v.booleano_ : padrao;
}

void Json::escapar(const std::string& texto, std::string& saida) {
    saida.push_back('"');
    for (const unsigned char c : texto) {
        switch (c) {
            case '"':  saida += "\\\""; break;
            case '\\': saida += "\\\\"; break;
            case '\b': saida += "\\b";  break;
            case '\f': saida += "\\f";  break;
            case '\n': saida += "\\n";  break;
            case '\r': saida += "\\r";  break;
            case '\t': saida += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    saida += buffer;
                } else {
                    // UTF-8 passa cru: é o que o outro lado espera.
                    saida.push_back(static_cast<char>(c));
                }
        }
    }
    saida.push_back('"');
}

std::string Json::paraTexto() const {
    std::string saida;
    switch (tipo_) {
        case Tipo::Nulo:
            saida = "null";
            break;
        case Tipo::Booleano:
            saida = booleano_ ? "true" : "false";
            break;
        case Tipo::Numero: {
            if (numero_ == static_cast<double>(static_cast<int64_t>(numero_)) &&
                std::fabs(numero_) < 9.0e15) {
                // Inteiro sai como inteiro: carimbo de tempo em notação
                // científica quebra o Date do navegador do outro lado.
                saida = std::to_string(static_cast<int64_t>(numero_));
            } else {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%.17g", numero_);
                saida = buffer;
            }
            break;
        }
        case Tipo::Texto:
            escapar(texto_, saida);
            break;
        case Tipo::Lista: {
            saida.push_back('[');
            for (size_t i = 0; i < lista_.size(); ++i) {
                if (i) saida.push_back(',');
                saida += lista_[i].paraTexto();
            }
            saida.push_back(']');
            break;
        }
        case Tipo::Objeto: {
            saida.push_back('{');
            bool primeiro = true;
            for (const auto& [chave, valor] : objeto_) {
                if (!primeiro) saida.push_back(',');
                primeiro = false;
                escapar(chave, saida);
                saida.push_back(':');
                saida += valor.paraTexto();
            }
            saida.push_back('}');
            break;
        }
    }
    return saida;
}

Json Json::analisar(std::string_view bruto) {
    Leitor leitor{bruto};
    Json valor = lerValor(leitor, 0);
    if (leitor.erro) return Json{};
    return valor;
}

}  // namespace gl
