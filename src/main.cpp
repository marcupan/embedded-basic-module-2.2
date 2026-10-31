// ═══════════════════════════════════════════════════════════════════════════
// Домашка 2.2 · час спрацювання реле й транзистора, метод опитування
// Плата: YD-ESP32-S3 (ESP32-S3-WROOM-1-N16R8) · PlatformIO · Arduino framework
// Карта реле: map.svg · тимчасова схема транзистора: map-transistor.svg
//
// Клавіші в моніторі порту:
//   r — етап 3: реле (ключ BC547B + модуль K-24 + контакт NO/COM)
//   t — етап 2: транзистор окремо (тимчасова схема з повторювачем на емітері)
//   ? — підказка
//
// Метод: start = micros() → digitalWrite → опитуємо вхід, поки рівень не зміниться → end = micros().
// Брязкіт: беремо перший фронт, а до наступної команди чекаємо, поки контакт заспокоїться.
// ═══════════════════════════════════════════════════════════════════════════
#include <Arduino.h>

namespace {
    struct Config {
        static constexpr uint8_t RELAY_CTRL_PIN = 12; // база BC547B через 10 кОм: HIGH = реле увімкнене
        static constexpr uint8_t RELAY_SENSE_PIN = 11; // NO реле, COM на землі: LOW = контакт замкнений
        static constexpr uint8_t TRANS_CTRL_PIN = 10; // база тимчасового BC547B через 10 кОм
        static constexpr uint8_t TRANS_SENSE_PIN = 3; // емітер: HIGH = транзистор відкритий
        static constexpr uint8_t MOTOR_PIN = 13; // тримаємо вимкненим, щоб не заважав вимірам

        static constexpr uint8_t SAMPLES = 20;
        static constexpr uint32_t TIMEOUT_NONE = UINT32_MAX;
    };

    struct Channel {
        const char *name;
        uint8_t ctrlPin;
        uint8_t sensePin;
        bool activeLevel; // що читає вхід, коли ключ або контакт спрацював
        uint32_t timeoutUs;
        uint32_t settleMs; // пауза між командами: у реле за цей час стихає брязкіт
    };

    const Channel RELAY{"реле", Config::RELAY_CTRL_PIN, Config::RELAY_SENSE_PIN, LOW, 200000, 300};
    const Channel TRANSISTOR{"транзистор", Config::TRANS_CTRL_PIN, Config::TRANS_SENSE_PIN, HIGH, 5000, 5};

    struct Stats {
        uint32_t count = 0;
        uint32_t timeouts = 0;
        uint64_t sum = 0;
        uint32_t minUs = UINT32_MAX;
        uint32_t maxUs = 0;

        void add(const uint32_t us) {
            if (us == Config::TIMEOUT_NONE) {
                timeouts++;
                return;
            }
            count++;
            sum += us;
            minUs = min(minUs, us);
            maxUs = max(maxUs, us);
        }

        float average() const { return count > 0 ? static_cast<float>(sum) / count : 0.0f; }
    };

    // Останні середні: щоб після обох етапів порахувати чистий час реле
    float transOnAvg = -1.0f;
    float transOffAvg = -1.0f;
    float relayOnAvg = -1.0f;
    float relayOffAvg = -1.0f;

    void printHelp() {
        Serial.println("r — виміряти реле · t — виміряти транзистор · ? — підказка");
    }

    // Між start і end лише команда й опитування: Serial тут додав би сотні мікросекунд
    uint32_t measureEdge(const Channel &ch, const bool command, const bool expected) {
        const uint32_t start = micros();
        digitalWrite(ch.ctrlPin, command ? HIGH : LOW);
        while (digitalRead(ch.sensePin) != expected) {
            if (micros() - start >= ch.timeoutUs) return Config::TIMEOUT_NONE;
        }
        return micros() - start;
    }

    void printValue(const uint32_t us) {
        if (us == Config::TIMEOUT_NONE) {
            Serial.print("ТАЙМАУТ");
        } else {
            Serial.printf("%lu мкс", static_cast<unsigned long>(us));
        }
    }

    void printStats(const char *label, const Stats &stats) {
        Serial.printf("%s: середнє %.1f мкс · мін %lu · макс %lu · вимірів %lu · таймаутів %lu\n",
                      label,
                      stats.average(),
                      static_cast<unsigned long>(stats.count > 0 ? stats.minUs : 0),
                      static_cast<unsigned long>(stats.maxUs),
                      static_cast<unsigned long>(stats.count),
                      static_cast<unsigned long>(stats.timeouts));
    }

    void printRelayOnly() {
        if (transOnAvg < 0 || relayOnAvg < 0) return;
        Serial.printf("Чисте реле: увімкнення %.1f мкс · вимкнення %.1f мкс (реле мінус транзистор)\n",
                      relayOnAvg - transOnAvg,
                      relayOffAvg - transOffAvg);
    }

    // delay тут навмисно: виміри йдуть один за одним, більше нічого робити не треба
    void runSeries(const Channel &ch, float &onAvg, float &offAvg) {
        digitalWrite(ch.ctrlPin, LOW);
        delay(ch.settleMs);
        if (digitalRead(ch.sensePin) == ch.activeLevel) {
            Serial.printf("%s: ключ вимкнений, а вхід уже показує спрацювання. Перевір дроти\n", ch.name);
            return;
        }

        Serial.printf("── %s · %u вимірів ──\n", ch.name, Config::SAMPLES);
        Stats on;
        Stats off;
        for (uint8_t i = 1; i <= Config::SAMPLES; i++) {
            const uint32_t onUs = measureEdge(ch, true, ch.activeLevel);
            delay(ch.settleMs);
            const uint32_t offUs = measureEdge(ch, false, !ch.activeLevel);
            delay(ch.settleMs);

            on.add(onUs);
            off.add(offUs);
            Serial.printf("%2u | увімк ", i);
            printValue(onUs);
            Serial.print(" | вимк ");
            printValue(offUs);
            Serial.println();
        }

        printStats("Увімкнення", on);
        printStats("Вимкнення ", off);
        // Серія з таймаутами не йде в підрахунок чистого часу
        const bool clean = on.timeouts == 0 && off.timeouts == 0;
        onAvg = clean ? on.average() : -1.0f;
        offAvg = clean ? off.average() : -1.0f;
        printRelayOnly();
    }

    void handleKeys() {
        while (Serial.available() > 0) {
            const char key = static_cast<char>(Serial.read());
            if (key == 'r') {
                runSeries(RELAY, relayOnAvg, relayOffAvg);
            } else if (key == 't') {
                runSeries(TRANSISTOR, transOnAvg, transOffAvg);
            } else if (key == '?') {
                printHelp();
            }
        }
    }
} // namespace

void setup() {
    // Спершу рівень, потім режим: щоб реле не клацнуло на старті
    digitalWrite(Config::RELAY_CTRL_PIN, LOW);
    pinMode(Config::RELAY_CTRL_PIN, OUTPUT);
    digitalWrite(Config::TRANS_CTRL_PIN, LOW);
    pinMode(Config::TRANS_CTRL_PIN, OUTPUT);
    digitalWrite(Config::MOTOR_PIN, LOW);
    pinMode(Config::MOTOR_PIN, OUTPUT);

    pinMode(Config::RELAY_SENSE_PIN, INPUT_PULLUP);
    pinMode(Config::TRANS_SENSE_PIN, INPUT); // рівень задає резистор на емітері, внутрішня підтяжка завадила б

    Serial.begin(115200);
    delay(1500); // час на підключення монітора (рідний USB)
    Serial.println("Домашка 2.2 · час реле й транзистора");
    printHelp();
}

void loop() {
    handleKeys();
}
