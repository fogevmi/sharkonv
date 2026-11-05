/* Шифры для защищённых PDF: все функции работают с байтами в std::string. */
#ifndef FB2C_PDFCRYPT_H
#define FB2C_PDFCRYPT_H

#include <string>

std::string md5(const std::string& data);
std::string sha256(const std::string& data);
std::string sha384(const std::string& data);
std::string sha512(const std::string& data);
std::string rc4(const std::string& key, const std::string& data);
/* ключ 16 или 32 байта, IV 16 байт, длина данных кратна 16 (остаток отбрасывается), без набивки */
std::string aesCbcEncrypt(const std::string& key, const std::string& iv, const std::string& data);
std::string aesCbcDecrypt(const std::string& key, const std::string& iv, const std::string& data);

#endif
