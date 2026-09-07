#ifndef WEB_ADMIN_FONTS_H
#define WEB_ADMIN_FONTS_H

#include <string>

// Inter 4.1 is bundled so the admin UI uses the same typeface on Windows,
// iOS, and the physical display without depending on an internet CDN.
void appendWebFontFaceStyles(std::string& html);


#endif  // WEB_ADMIN_FONTS_H
