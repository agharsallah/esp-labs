// wifi-lab-w2: no remote link. Flash wifi-lab-w2-remote for it.
#include "../remote.h"

namespace remote {

bool builtIn() { return false; }
void begin(const ApiRules &, const ApiToken &) {}
void setUrl(const String &) {}
String url() { return ""; }
State state() { return State::Off; }
void publish(const char *, const String &) {}

}  // namespace remote
