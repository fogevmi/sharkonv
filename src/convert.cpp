#include "convert.h"

bool isSupportedInput(const std::string& name) {
    std::string e = pathExt(name);
    return e == "fb2" || e == "txt" || e == "html" || e == "htm" || e == "xhtml" ||
           e == "docx" || e == "doc" || e == "odt" || e == "rtf" || e == "pdf" || e == "djvu" || e == "djv" || e == "epub" || e == "mobi" || e == "azw" || e == "azw3" ||
           e == "prc" || e == "zip" || e == "fbz";
}

bool isSupportedTarget(const std::string& name) {
    std::string e = lowerAscii(name);
    return e == "fb2" || e == "epub" || e == "html" || e == "txt" || e == "mobi";
}

std::string resolveTarget(const std::string& inputName, const std::string& selected) {
    if (!selected.empty() && lowerAscii(selected) != "auto") return lowerAscii(selected);
    if (pathExt(inputName) == "fb2") return "epub";
    return "fb2";
}
