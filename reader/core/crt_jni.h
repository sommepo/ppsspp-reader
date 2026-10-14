// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <jni.h>
#include <string>
#include "reader/core/crt.h"

/**
 * The CRT picture for the Android apps: what the Kotlin settings page (reader/ui/
 * CrtSettings.kt, the same file in every fork) asks of the native side. The same header in
 * every fork; a fork's own bridge file names its Java package once:
 *
 *     READER_CRT_JNI(org_example_app)   // for the class org.example.app.reader.ui.CrtNative
 */
namespace Reader::Crt::Jni {

/// The table as lines the settings page reads: a line a row, its fields between tabs (key,
/// name, heading, hint, kind, lowest, highest, step, first value, choices), then a line
/// "#look", key, name for every ready-made look.
inline std::string Table() {
    std::string text;
    for (const Param& param : Params()) {
        const char* kind = param.kind == Kind::Toggle ? "toggle"
                           : param.kind == Kind::Choice ? "choice" : "slider";
        text += param.key;
        text += '\t';
        text += param.name;
        text += '\t';
        text += param.group;
        text += '\t';
        text += param.hint;
        text += '\t';
        text += kind;
        for (const float number : {param.low, param.high, param.step, param.initial}) {
            text += '\t';
            // Thousandths, as the kept text writes a number.
            const long thousandths = static_cast<long>(number * 1000.f + (number < 0 ? -0.5f : 0.5f));
            const long size = thousandths < 0 ? -thousandths : thousandths;
            text += (thousandths < 0 ? "-" : "") + std::to_string(size / 1000) + "." +
                    std::to_string(1000 + size % 1000).substr(1);
        }
        text += '\t';
        text += param.choices != nullptr ? param.choices : "";
        text += '\n';
    }
    for (const Preset& preset : Presets()) {
        text += "#look\t";
        text += preset.key;
        text += '\t';
        text += preset.name;
        text += '\n';
    }
    return text;
}

inline std::string Text(JNIEnv* env, jstring text) {
    if (text == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string result = chars != nullptr ? chars : "";
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(text, chars);
    }
    return result;
}

} // namespace Reader::Crt::Jni

#define READER_CRT_JNI(package)                                                                    \
    extern "C" {                                                                                   \
    JNIEXPORT jstring JNICALL Java_##package##_reader_ui_CrtNative_table(JNIEnv* env, jobject) {   \
        return env->NewStringUTF(Reader::Crt::Jni::Table().c_str());                               \
    }                                                                                              \
    JNIEXPORT void JNICALL Java_##package##_reader_ui_CrtNative_apply(JNIEnv* env, jobject,        \
                                                                      jstring text) {              \
        Reader::Crt::Deserialize(Reader::Crt::Jni::Text(env, text));                               \
    }                                                                                              \
    JNIEXPORT jstring JNICALL Java_##package##_reader_ui_CrtNative_values(JNIEnv* env, jobject) {  \
        return env->NewStringUTF(Reader::Crt::Serialize().c_str());                                \
    }                                                                                              \
    JNIEXPORT jboolean JNICALL Java_##package##_reader_ui_CrtNative_look(JNIEnv* env, jobject,     \
                                                                         jstring key) {            \
        return Reader::Crt::ApplyPreset(Reader::Crt::Jni::Text(env, key).c_str()) ? JNI_TRUE       \
                                                                                  : JNI_FALSE;     \
    }                                                                                              \
    JNIEXPORT jstring JNICALL Java_##package##_reader_ui_CrtNative_currentLook(JNIEnv* env,        \
                                                                               jobject) {          \
        return env->NewStringUTF(Reader::Crt::CurrentPreset().c_str());                            \
    }                                                                                              \
    }
