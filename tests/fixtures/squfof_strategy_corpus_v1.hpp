#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gnfs::tests::fixtures {

struct SqufofStrategyCase {
    uint64_t n;
    uint32_t max_iterations;
};

inline constexpr std::string_view FIXED_50D_SQUFOF_STRATEGY_V1_NAME =
    "fixed_50d_squfof_strategy_v1";

// Provenance boundaries are indices into FIXED_50D_SQUFOF_STRATEGY_V1.
// The first segment is the exact call order observed at SQUFOF::factor while
// running the fixed 50-digit candidate sweep's serial oracle. The second is
// the exact alternating high-cofactor/residual call order from the existing
// hard-3LP test, which exercises the same production classify helper. The
// final segment freezes values from the existing deterministic seed-42,
// 60-bit ECM benchmark generator and assigns the production 3LP/main-2LP
// budgets. No random generation occurs when this fixture is consumed.
inline constexpr size_t FIXED_50D_SQUFOF_STRATEGY_V1_SERIAL_ORACLE_COUNT = 159;
inline constexpr size_t FIXED_50D_SQUFOF_STRATEGY_V1_HARD_3LP_COUNT = 12;
inline constexpr size_t FIXED_50D_SQUFOF_STRATEGY_V1_HIGH_BAND_SUPPLEMENT_COUNT = 21;

inline constexpr std::array<SqufofStrategyCase, 192> FIXED_50D_SQUFOF_STRATEGY_V1{{
    // Exact SQUFOF calls from the fixed 50-digit first-batch serial oracle.
    SqufofStrategyCase{UINT64_C(702241783543), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(14917492623923), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(17408107217257), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(9080265143), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(16515156321617), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(6039407812811), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(2673181962863), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(56657288341), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(42785800778911), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1084119233653), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(48023164282271), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(2089919427457), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(51267524170987), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(2861823546793), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(11440763308573), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(43850865229723), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(4028276553347), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(200054392651), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(11350844179139), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1397616228161), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(722109085921), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(19433255883259), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(75286712041), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(2188023838697), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1723468800761), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(74526814673), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(40498873579), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(9043483344997), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(4241548145701), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(65916792931343), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(320924251517), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(59560268137), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(2221467278269), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(34489479889), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(7786184710099), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(44732328590231), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1440179006027), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(3565874862857), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(34002702599), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(7618144681531), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(2506853477209), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(3610631537581), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(30349441907), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(1493482052791), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(148657261769), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(1078277936117), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(43314547681), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(197313754553), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(69788415450379), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(20172901736873), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(19171397939), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(25661309793797), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(171836503331), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(7615832037599), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(14615726418599), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(9944226607489), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(3533426325073), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(976499710139), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(91080288499), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(5416384318921), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(14067103089089), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(3542482451489), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(930147006589), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(3822647699569), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(17214259359809), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(3739119025517), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(188551004063), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(282661108831), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(410272665331), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(10166124185729), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(37003183303), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(725828363723), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(37305936169), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(250576987109), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(3770862678127), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(38415351691), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(8281948767167), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(3827380629887), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(130945435927), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(903306163657), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(337022715853), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(32960353301489), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(23001016337099), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(4865468954119), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(766881903523), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(2157751236107), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1795149629279), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(222480396581), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(3097888398167), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(95954973251), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(16433577726011), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(11245976738209), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(2603538030089), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(15654464317), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(392366563661), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(8132060345779), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(52233429004571), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(20537230962623), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(19649218794431), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(12440601618287), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(38179938067), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(1191874642991), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(41476573387303), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(156598452083), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(2996627919199), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(47344186533403), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(2199391370611), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(3210088528331), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(11969572790023), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(10054343377277), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1453873807381), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(47482071343117), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(384394920653), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(512395319729), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(7684973606699), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(113884151441), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(2826127363357), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(24712426221719), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(17360339075081), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(193353876787), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(2118909644881), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(964640255431), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(63269059690337), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1127369381183), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(50237323783), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(1539084312989), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(3882974096809), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1278369600301), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(53301822698569), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(10307001930679), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(5277218600389), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(84613601777), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(380192436067), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(11534522580671), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(33007222016551), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(908368347989), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(2443456869421), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(366257190359), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(35617589862989), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(34076865268753), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(3638907232189), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(13174278697), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(4798275870823), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(658182758237), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(1162795167637), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(923987420879), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(12267453658483), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(16772917688591), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(54600063213323), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1223356539011), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(510729841507), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(56927328425311), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(7381997720939), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(90612487949), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(126360800609), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(1000719061219), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(7788296258263), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(934234182953), UINT32_C(2000)},
    SqufofStrategyCase{UINT64_C(21842238644027), UINT32_C(5000)},

    // Exact calls from the existing hard-3LP production-helper test flow.
    SqufofStrategyCase{UINT64_C(1000073001431003663), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1000040000111), UINT32_C(1000)},
    SqufofStrategyCase{UINT64_C(1000219015039312741), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1000120003159), UINT32_C(1000)},
    SqufofStrategyCase{UINT64_C(1000371045812882881), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1000250015561), UINT32_C(1000)},
    SqufofStrategyCase{UINT64_C(1000481077023105539), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1000310024009), UINT32_C(1000)},
    SqufofStrategyCase{UINT64_C(1000563105637604653), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1000376035319), UINT32_C(1000)},
    SqufofStrategyCase{UINT64_C(1000623129327943657), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(1000412042387), UINT32_C(1000)},

    // Frozen high-band supplements from the existing deterministic seed-42 ECM test corpus.
    SqufofStrategyCase{UINT64_C(612100043677452881), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(481402312577721317), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(575273664663040037), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(819045352664884537), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(661884611963511361), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(624392203886711573), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(910783029331139707), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(677924676258810797), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(816040518942301063), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(718167574456252909), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(706661573091841091), UINT32_C(5000)},
    SqufofStrategyCase{UINT64_C(612100043677452881), UINT32_C(20000)},
    SqufofStrategyCase{UINT64_C(481402312577721317), UINT32_C(20000)},
    SqufofStrategyCase{UINT64_C(575273664663040037), UINT32_C(20000)},
    SqufofStrategyCase{UINT64_C(819045352664884537), UINT32_C(20000)},
    SqufofStrategyCase{UINT64_C(661884611963511361), UINT32_C(20000)},
    SqufofStrategyCase{UINT64_C(624392203886711573), UINT32_C(20000)},
    SqufofStrategyCase{UINT64_C(910783029331139707), UINT32_C(20000)},
    SqufofStrategyCase{UINT64_C(677924676258810797), UINT32_C(20000)},
    SqufofStrategyCase{UINT64_C(816040518942301063), UINT32_C(20000)},
    SqufofStrategyCase{UINT64_C(706661573091841091), UINT32_C(20000)},
}};

// Digest encoding:
//   corpus name bytes, zero separator, little-endian case count, then for
//   every case its little-endian index, n, and max_iterations. The two lanes
//   use the same stable mixer as the fixed candidate fixture.
inline constexpr uint64_t FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_LOW = UINT64_C(11585003526353080300);
inline constexpr uint64_t FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_HIGH = UINT64_C(16066302168872607439);

namespace detail {

struct SqufofStrategyDigest {
    uint64_t low;
    uint64_t high;
};

class SqufofStrategyDigestBuilder {
public:
    constexpr void append_byte(uint8_t value) noexcept {
        low_ ^= static_cast<uint64_t>(value);
        low_ *= UINT64_C(1099511628211);

        high_ ^= static_cast<uint64_t>(value) + byte_index_ * UINT64_C(0x9e3779b97f4a7c15);
        high_ = std::rotl(high_, 27);
        high_ *= UINT64_C(0x94d049bb133111eb);
        high_ += UINT64_C(0x2545f4914f6cdd1d);
        ++byte_index_;
    }

    constexpr void append_u32(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    constexpr void append_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    [[nodiscard]] constexpr SqufofStrategyDigest finish() const noexcept {
        return {
            avalanche(low_ ^ byte_index_),
            avalanche(high_ ^ std::rotl(byte_index_, 17)),
        };
    }

private:
    [[nodiscard]] static constexpr uint64_t avalanche(uint64_t value) noexcept {
        value ^= value >> 30;
        value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27;
        value *= UINT64_C(0x94d049bb133111eb);
        value ^= value >> 31;
        return value;
    }

    uint64_t low_ = UINT64_C(14695981039346656037);
    uint64_t high_ = UINT64_C(0x243f6a8885a308d3);
    uint64_t byte_index_ = 0;
};

[[nodiscard]] constexpr SqufofStrategyDigest
compute_fixed_50d_squfof_strategy_v1_digest() noexcept {
    SqufofStrategyDigestBuilder builder;
    for (char value : FIXED_50D_SQUFOF_STRATEGY_V1_NAME) {
        builder.append_byte(static_cast<uint8_t>(value));
    }
    builder.append_byte(0);
    builder.append_u64(FIXED_50D_SQUFOF_STRATEGY_V1.size());
    for (size_t index = 0; index < FIXED_50D_SQUFOF_STRATEGY_V1.size(); ++index) {
        const auto& test_case = FIXED_50D_SQUFOF_STRATEGY_V1[index];
        builder.append_u64(index);
        builder.append_u64(test_case.n);
        builder.append_u32(test_case.max_iterations);
    }
    return builder.finish();
}

} // namespace detail

inline constexpr auto FIXED_50D_SQUFOF_STRATEGY_V1_COMPUTED_DIGEST =
    detail::compute_fixed_50d_squfof_strategy_v1_digest();

static_assert(FIXED_50D_SQUFOF_STRATEGY_V1_COMPUTED_DIGEST.low ==
              FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_LOW);
static_assert(FIXED_50D_SQUFOF_STRATEGY_V1_COMPUTED_DIGEST.high ==
              FIXED_50D_SQUFOF_STRATEGY_V1_DIGEST_HIGH);
static_assert(FIXED_50D_SQUFOF_STRATEGY_V1_SERIAL_ORACLE_COUNT +
                  FIXED_50D_SQUFOF_STRATEGY_V1_HARD_3LP_COUNT +
                  FIXED_50D_SQUFOF_STRATEGY_V1_HIGH_BAND_SUPPLEMENT_COUNT ==
              FIXED_50D_SQUFOF_STRATEGY_V1.size());

} // namespace gnfs::tests::fixtures
