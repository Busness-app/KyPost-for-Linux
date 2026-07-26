#include "security/PinPolicy.h"

namespace PinPolicy {

Rejection validate(const QString& pin)
{
    if (pin.size() < kMinimumLength)
        return Rejection::TooShort;
    if (pin.size() > kMaximumLength)
        return Rejection::TooLong;

    bool allSame = true;
    for (int i = 1; i < pin.size(); ++i) {
        if (pin.at(i) != pin.at(0)) {
            allSame = false;
            break;
        }
    }
    if (allSame)
        return Rejection::AllSameCharacter;

    // Runs of consecutive code points in either direction. Checked over the
    // whole string rather than a hardcoded "123456" list so "345678" and
    // "98765432" are caught too.
    bool ascending = true;
    bool descending = true;
    for (int i = 1; i < pin.size(); ++i) {
        const int delta = pin.at(i).unicode() - pin.at(i - 1).unicode();
        if (delta != 1)
            ascending = false;
        if (delta != -1)
            descending = false;
    }
    if (ascending || descending)
        return Rejection::Sequential;

    return Rejection::Ok;
}

} // namespace PinPolicy
