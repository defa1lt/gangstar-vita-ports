#ifndef RIO_CONTROLLER_LABELS_H
#define RIO_CONTROLLER_LABELS_H

#include <stddef.h>
#include <string.h>

static inline int controller_tutorial_text(char *output, size_t capacity,
                                            const char *input, unsigned index,
                                            int russian) {
    static const char *const english[][2] = {
        {"right and left triggers", "R / L"},
        {"right trigger", "R"}, {"left trigger", "L"},
        {"A button", "cross button"}, {"B button", "circle button"},
        {"X button", "square button"}, {"Y button", "triangle button"},
    };
    static const char *const russian_labels[][2] = {
        {"правый и левый курки", "R / L"},
        {"правый курок", "R"}, {"левый курок", "L"},
        {"кнопку A", "крестик"}, {"кнопку B", "круг"},
        {"кнопку X", "квадрат"}, {"кнопку Y", "треугольник"},
        {"кнопку А", "крестик"},
    };
    const char *const (*labels)[2] = russian ? russian_labels : english;
    size_t count = russian ? sizeof(russian_labels) / sizeof(russian_labels[0])
                           : sizeof(english) / sizeof(english[0]);
    size_t written = 0;
    int changed = 0;
    if (!output || !capacity || !input) return -1;
    while (*input) {
        const char *replacement = input;
        size_t consumed = 1, length = 1;
        if (index >= 625 && index <= 652) {
            for (size_t i = 0; i < count; ++i) {
                size_t token_length = strlen(labels[i][0]);
                if (strncmp(input, labels[i][0], token_length) != 0) continue;
                replacement = labels[i][1];
                /* Road vehicles use shoulders; aircraft retain face-button thrust. */
                if ((index == 628 || index == 648) &&
                    (i == 3 || i == 4 || (russian && i == 7)))
                    replacement = i == 4 ? "L" : "R";
                consumed = token_length;
                length = strlen(replacement);
                changed = 1;
                break;
            }
        }
        if (length >= capacity - written) {
            output[0] = '\0';
            return -1;
        }
        memcpy(output + written, replacement, length);
        written += length;
        input += consumed;
    }
    output[written] = '\0';
    return changed;
}

#endif
