#ifndef FB2C_CONVERT_H
#define FB2C_CONVERT_H

#include "book.h"

bool readBook(const std::string& fileName, const Bytes& data, Book& out, std::string* err);
bool writeBook(Book& book, const std::string& target, Bytes& out, std::string* err);

bool isSupportedInput(const std::string& name);
bool isSupportedTarget(const std::string& name);

/* "auto" -> epub для fb2, иначе fb2 */
std::string resolveTarget(const std::string& inputName, const std::string& selected);

#endif
