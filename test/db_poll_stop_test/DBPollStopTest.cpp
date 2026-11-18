#include <tm_kit/basic/SerializationHelperMacros.hpp>
#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/transport/db_table_importer_exporter/DBTableImporterFactory.hpp>

#include <soci/sqlite3/soci-sqlite3.h>

#include <chrono>
#include <future>
#include <iostream>
#include <thread>

using namespace dev::cd606::tm;
using namespace std::chrono_literals;

#define PollRowFields ((int, value))
TM_BASIC_CBOR_CAPABLE_STRUCT(PollRow, PollRowFields);
TM_BASIC_CBOR_CAPABLE_STRUCT_SERIALIZE_NO_FIELD_NAMES(PollRow, PollRowFields);

namespace
{
    struct Clock {
        using TimePointType = std::chrono::system_clock::time_point;
        using DurationType = TimePointType::duration;
        static constexpr bool PreserveInputRelativeOrder = false;
        static constexpr bool CanBeActualTimeClock = true;
        TimePointType now()
        {
            return std::chrono::system_clock::now();
        }
        TimePointType resolveTime()
        {
            return now();
        }
        TimePointType resolveTime(TimePointType const &t)
        {
            return t;
        }
        void log(infra::LogLevel, std::string const &)
        {
        }
    };
    using Env = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, Clock>;
    using M = infra::RealTimeApp<Env>;
    using Factory = transport::db_table_importer_exporter::DBTableImporterFactory<M>;

    bool checkPolling(std::shared_ptr<soci::session> const &session, bool withKey)
    {
        Env env;
        std::promise<bool> firstQuery;
        auto firstQueryFuture = firstQuery.get_future();
        std::size_t batches = 0;
        auto sink = M::pureExporter<std::vector<PollRow>>([&](std::vector<PollRow> &&rows) {
            if (++batches == 1) {
                firstQuery.set_value(rows.size() == 1 && rows.front().value == 7);
            }
        });
        auto importer = withKey ? Factory::createRepeatedImporterWithKeyCheck<PollRow>(session, "items", 1h, [](PollRow const &row) { return row.value; })
                                : Factory::createRepeatedImporter<PollRow>(session, "items", 1h);
        auto runner = std::make_unique<infra::AppRunner<M>>(&env);
        runner->connect(runner->importItem("database", importer), runner->exporterAsSink("sink", sink));
        runner->finalize();
        bool const firstRowDelivered = firstQueryFuture.wait_for(3s) == std::future_status::ready && firstQueryFuture.get();
        std::this_thread::sleep_for(20ms); // Allow the worker to enter its next poll wait.
        auto const begin = std::chrono::steady_clock::now();
        importer.reset();
        runner.reset();
        bool const ok = firstRowDelivered && batches == 1 && std::chrono::steady_clock::now() - begin < 2s;
        std::cout << (ok ? "ok: " : "FAIL: ") << (withKey ? "keyed" : "plain") << " importer delivers its first row and stops during an hour-long poll interval" << std::endl;
        return ok;
    }
} // namespace

int main()
{
    auto session = std::make_shared<soci::session>(
#ifdef _MSC_VER
        *soci::factory_sqlite3(),
#else
        soci::sqlite3,
#endif
        ":memory:");
    *session << "CREATE TABLE items (value INTEGER)";
    *session << "INSERT INTO items VALUES (7)";
    // A new instance must always poll immediately and start with no seen keys.
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        ok = checkPolling(session, false) && ok;
        ok = checkPolling(session, true) && ok;
    }
    return ok ? 0 : 1;
}
