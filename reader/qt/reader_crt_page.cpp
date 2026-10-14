// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include "reader/core/crt.h"
#include "reader/desktop/screen_text.h"
#include "reader/desktop/settings.h"
#include "reader/qt/reader_crt_page.h"

namespace ReaderCrtPage {

const char* const SettingsKey = "picture/crt";

namespace {

QString Tr(const char* text) {
    return QCoreApplication::translate("ReaderUi", text);
}

int Steps(const Reader::Crt::Param& param) {
    return static_cast<int>(std::lround((param.high - param.low) / param.step));
}

} // namespace

void Load(const ReaderApp::ReaderSettings& settings) {
    // What is laid over the game's text follows the glass.
    static bool bound = false;
    if (!bound) {
        bound = true;
        ReaderApp::PlacedText::SetBend([](float& x, float& y) { Reader::Crt::ToScreen(x, y); });
    }
    Reader::Crt::Deserialize(settings.Value(QString::fromLatin1(SettingsKey)).toString().toStdString());
}

Page Make(ReaderApp::ReaderSettings& settings, QWidget* parent) {
    namespace Crt = Reader::Crt;
    const std::string before = Crt::Serialize();

    auto* page = new QWidget(parent);
    auto* outer = new QVBoxLayout(page);

    // What puts every control on the page back in step with the values, after a ready-made
    // look was chosen. `quiet` keeps the controls from writing values while they are set.
    auto refreshers = std::make_shared<std::vector<std::function<void()>>>();
    auto quiet = std::make_shared<bool>(false);
    auto* looks = new QComboBox;
    const auto refresh = [refreshers, quiet, looks] {
        *quiet = true;
        for (const auto& each : *refreshers) {
            each();
        }
        const int index = looks->findData(QString::fromStdString(Crt::CurrentPreset()));
        looks->setCurrentIndex(index < 0 ? looks->count() - 1 : index);
        *quiet = false;
    };
    const auto changed = [quiet, looks] {
        if (*quiet) {
            return;
        }
        *quiet = true;
        const int index = looks->findData(QString::fromStdString(Crt::CurrentPreset()));
        looks->setCurrentIndex(index < 0 ? looks->count() - 1 : index);
        *quiet = false;
    };

    auto* top = new QFormLayout;
    outer->addLayout(top);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget;
    auto* form = new QFormLayout(inner);
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    const char* group = "";
    for (const Crt::Param& param : Crt::Params()) {
        const char* key = param.key;
        const QString name = Tr(param.name);
        const QString hint = Tr(param.hint);
        const bool first = std::strcmp(key, "enabled") == 0;
        QFormLayout* target = first ? top : form;
        if (!first && std::strcmp(group, param.group) != 0) {
            group = param.group;
            auto* heading = new QLabel(QStringLiteral("<b>%1</b>").arg(Tr(group)));
            heading->setContentsMargins(0, 8, 0, 0);
            form->addRow(heading);
        }
        switch (param.kind) {
        case Crt::Kind::Toggle: {
            auto* box = new QCheckBox(name);
            box->setToolTip(hint);
            target->addRow(box);
            QObject::connect(box, &QCheckBox::toggled, page, [key, changed](bool on) {
                Crt::Set(key, on ? 1.f : 0.f);
                changed();
            });
            refreshers->push_back([box, key] { box->setChecked(Crt::Get(key) >= 0.5f); });
            break;
        }
        case Crt::Kind::Choice: {
            auto* box = new QComboBox;
            box->setToolTip(hint);
            const QStringList entries = QString::fromUtf8(param.choices).split(QLatin1Char('|'));
            for (const QString& entry : entries) {
                const int equals = entry.indexOf(QLatin1Char('='));
                box->addItem(Tr(entry.mid(equals + 1).toUtf8().constData()),
                             entry.left(equals).toFloat());
            }
            target->addRow(name, box);
            QObject::connect(box, &QComboBox::currentIndexChanged, page, [box, key, changed](int) {
                Crt::Set(key, box->currentData().toFloat());
                changed();
            });
            refreshers->push_back([box, key] {
                const float value = Crt::Get(key);
                for (int i = 0; i < box->count(); i++) {
                    if (std::fabs(box->itemData(i).toFloat() - value) < 0.001f) {
                        box->setCurrentIndex(i);
                        break;
                    }
                }
            });
            break;
        }
        case Crt::Kind::Slider: {
            auto* slider = new QSlider(Qt::Horizontal);
            slider->setRange(0, Steps(param));
            slider->setPageStep(2);
            slider->setToolTip(hint);
            auto* number = new QLabel;
            number->setMinimumWidth(44);
            number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            auto* row = new QHBoxLayout;
            row->addWidget(slider, 1);
            row->addWidget(number);
            target->addRow(name, row);
            const float low = param.low;
            const float step = param.step;
            const auto show = [number, low, step](int at) {
                number->setText(QString::number(low + step * static_cast<float>(at), 'f', 2));
            };
            QObject::connect(slider, &QSlider::valueChanged, page, [key, low, step, show, changed](int at) {
                show(at);
                Crt::Set(key, low + step * static_cast<float>(at));
                changed();
            });
            refreshers->push_back([slider, key, low, step, show] {
                const int at = static_cast<int>(std::lround((Crt::Get(key) - low) / step));
                slider->setValue(at);
                show(at);
            });
            break;
        }
        }
    }

    for (const Crt::Preset& preset : Crt::Presets()) {
        looks->addItem(Tr(preset.name), QString::fromLatin1(preset.key));
    }
    looks->addItem(Tr("My own"), QString());
    looks->setToolTip(Tr("Ready-made pictures. Moving a slider makes the picture your own."));
    top->addRow(Tr("Look"), looks);
    QObject::connect(looks, &QComboBox::activated, page, [looks, refresh](int index) {
        const QString key = looks->itemData(index).toString();
        if (!key.isEmpty()) {
            Crt::ApplyPreset(key.toLatin1().constData());
        }
        refresh();
    });
    refresh();

    Page result;
    result.widget = page;
    result.save = [&settings] {
        settings.SetValue(QString::fromLatin1(SettingsKey), QString::fromStdString(Crt::Serialize()));
    };
    result.revert = [before] { Crt::Deserialize(before); };
    return result;
}

} // namespace ReaderCrtPage
