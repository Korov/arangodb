////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "Functions.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "ApplicationFeatures/LanguageFeature.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/AqlValueMaterializer.h"
#include "Aql/Expression.h"
#include "Aql/ExpressionContext.h"
#include "Aql/Function.h"
#include "Aql/Query.h"
#include "Aql/Range.h"
#include "Aql/V8Executor.h"
#include "Basics/Endian.h"
#include "Basics/Exceptions.h"
#include "Basics/HybridLogicalClock.h"
#include "Basics/Mutex.h"
#include "Basics/MutexLocker.h"
#include "Basics/NumberUtils.h"
#include "Basics/StringBuffer.h"
#include "Basics/StringUtils.h"
#include "Basics/Utf8Helper.h"
#include "Basics/VPackStringBufferAdapter.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/conversions.h"
#include "Basics/datetime.h"
#include "Basics/fpconv.h"
#include "Basics/hashes.h"
#include "Basics/system-functions.h"
#include "Basics/tri-strings.h"
#include "Geo/Ellipsoid.h"
#include "Geo/GeoJson.h"
#include "Geo/ShapeContainer.h"
#include "Geo/Utils.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchPDP.h"
#include "IResearch/VelocyPackHelper.h"
#include "Indexes/Index.h"
#include "Logger/Logger.h"
#include "Pregel/Conductor.h"
#include "Pregel/PregelFeature.h"
#include "Pregel/Worker.h"
#include "Random/UniformCharacter.h"
#include "Rest/Version.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "Ssl/SslInterface.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Context.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/ExecContext.h"
#include "V8/v8-vpack.h"
#include "V8Server/v8-collection.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Validators.h"
#include "VocBase/Methods/Collections.h"

#include "analysis/token_attributes.hpp"
#include "utils/levenshtein_utils.hpp"
#include "utils/ngram_match_utils.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <date/date.h>
#include <date/iso_week.h>
#include <date/tz.h>
#include <s2/s2loop.h>

#include <unicode/schriter.h>
#include <unicode/stsearch.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>

#include <velocypack/Collection.h>
#include <velocypack/Dumper.h>
#include <velocypack/Iterator.h>
#include <velocypack/StringRef.h>
#include <velocypack/velocypack-aliases.h>
#include <algorithm>

#ifdef __APPLE__
#include <regex>
#endif

#ifdef _WIN32
#include "Basics/win-utils.h"
#else
#include <arpa/inet.h>
#endif

using namespace arangodb;
using namespace arangodb::aql;
using namespace std::chrono;
using namespace date;

/*
- always specify your user facing function name MYFUNC in error generators
- errors are broadcasted like this:
    - Wrong parameter types: registerInvalidArgumentWarning(expressionContext,
"MYFUNC")
    - Generic errors: registerWarning(expressionContext, "MYFUNC",
TRI_ERROR_QUERY_INVALID_REGEX);
    - ICU related errors: if (U_FAILURE(status)) {
::registerICUWarning(expressionContext, "MYFUNC", status); }
    - close with: return AqlValue(AqlValueHintNull());
- specify the number of parameters you expect at least and at max using:
- if you support optional parameters, first check whether the count is
sufficient using parameters.size()
- fetch the values using:
  AqlValue value
  - Anonymous  = extractFunctionParameterValue(parameters, 0);
  - ::getBooleanParameter() if you expect a bool
  - Stringify() if you need a string.
  - ::extractKeys() if its an object and you need the keys
  - ::extractCollectionName() if you expect a collection
  - ::listContainsElement() search for a member
  - ::parameterToTimePoint / ::dateFromParameters get a time string as date.
- check the values whether they match your expectations i.e. using:
  - param.isNumber() then extract it using: param.toInt64()
- Available helper functions for working with parameters:
  - ::variance()
  - ::sortNumberList()
  - ::unsetOrKeep ()
  - ::getDocumentByIdentifier ()
  - ::mergeParameters()
  - ::flattenList()

- now do your work with the parameters
- build up a result using a VPackbuilder like you would with regular velocpyack.
- return it wrapping it into an AqlValue

 */

namespace {

/// @brief an empty AQL value
static AqlValue const emptyAqlValue;

#ifdef __APPLE__
std::regex const ipV4LeadingZerosRegex("^(.*?\\.)?0[0-9]+.*$", std::regex::optimize);
#endif

/// @brief mutex used to protect UUID generation
static Mutex uuidMutex;

enum DateSelectionModifier {
  INVALID = 0,
  MILLI,
  SECOND,
  MINUTE,
  HOUR,
  DAY,
  WEEK,
  MONTH,
  YEAR
};

static_assert(DateSelectionModifier::INVALID < DateSelectionModifier::MILLI,
              "incorrect date selection order");
static_assert(DateSelectionModifier::MILLI < DateSelectionModifier::SECOND,
              "incorrect date selection order");
static_assert(DateSelectionModifier::SECOND < DateSelectionModifier::MINUTE,
              "incorrect date selection order");
static_assert(DateSelectionModifier::MINUTE < DateSelectionModifier::HOUR,
              "incorrect date selection order");
static_assert(DateSelectionModifier::HOUR < DateSelectionModifier::DAY,
              "incorrect date selection order");
static_assert(DateSelectionModifier::DAY < DateSelectionModifier::WEEK,
              "incorrect date selection order");
static_assert(DateSelectionModifier::WEEK < DateSelectionModifier::MONTH,
              "incorrect date selection order");
static_assert(DateSelectionModifier::MONTH < DateSelectionModifier::YEAR,
              "incorrect date selection order");

/// @brief validates documents for duplicate attribute names
bool isValidDocument(VPackSlice slice) {
  slice = slice.resolveExternals();

  if (slice.isObject()) {
    std::unordered_set<VPackStringRef> keys;

    auto it = VPackObjectIterator(slice, true);

    while (it.valid()) {
      if (!keys.emplace(it.key().stringRef()).second) {
        // duplicate key
        return false;
      }

      // recurse into object values
      if (!isValidDocument(it.value())) {
        return false;
      }
      it.next();
    }
  } else if (slice.isArray()) {
    auto it = VPackArrayIterator(slice);

    while (it.valid()) {
      // recursively validate array values
      if (!isValidDocument(it.value())) {
        return false;
      }
      it.next();
    }
  }

  // all other types are considered valid
  return true;
}

void registerICUWarning(ExpressionContext* expressionContext,
                        char const* functionName, UErrorCode status) {
  std::string msg;
  msg.append("in function '");
  msg.append(functionName);
  msg.append("()': ");
  msg.append(arangodb::basics::Exception::FillExceptionString(TRI_ERROR_ARANGO_ICU_ERROR,
                                                              u_errorName(status)));
  expressionContext->registerWarning(TRI_ERROR_ARANGO_ICU_ERROR, msg.c_str());
}

/// @brief extract a function parameter from the arguments
inline AqlValue const& extractFunctionParameterValue(VPackFunctionParameters const& parameters,
                                                     size_t position) {
  if (position >= parameters.size()) {
    // parameter out of range
    return ::emptyAqlValue;
  }
  return parameters[position];
}

/// @brief convert a number value into an AqlValue
AqlValue numberValue(double value, bool nullify) {
  if (std::isnan(value) || !std::isfinite(value) || value == HUGE_VAL || value == -HUGE_VAL) {
    if (nullify) {
      // convert to null
      return AqlValue(AqlValueHintNull());
    }
    // convert to 0
    return AqlValue(AqlValueHintZero());
  }

  return AqlValue(AqlValueHintDouble(value));
}

/// @brief optimized version of datetime stringification
/// string format is hard-coded to YYYY-MM-DDTHH:MM:SS.XXXZ
AqlValue timeAqlValue(ExpressionContext* expressionContext, char const* AFN,
                      tp_sys_clock_ms const& tp, bool utc = true) {
  char formatted[24];

  year_month_day ymd{floor<days>(tp)};
  auto day_time = make_time(tp - sys_days(ymd));

  auto y = static_cast<int>(ymd.year());
  // quick basic check here for dates outside the allowed range
  if (y < 0 || y > 9999) {
    arangodb::aql::registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_DATE_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  formatted[0] = '0' + (y / 1000);
  formatted[1] = '0' + ((y % 1000) / 100);
  formatted[2] = '0' + ((y % 100) / 10);
  formatted[3] = '0' + (y % 10);
  formatted[4] = '-';
  auto m = static_cast<unsigned>(ymd.month());
  formatted[5] = '0' + (m / 10);
  formatted[6] = '0' + (m % 10);
  formatted[7] = '-';
  auto d = static_cast<unsigned>(ymd.day());
  formatted[8] = '0' + (d / 10);
  formatted[9] = '0' + (d % 10);
  formatted[10] = 'T';
  auto h = day_time.hours().count();
  formatted[11] = '0' + (h / 10);
  formatted[12] = '0' + (h % 10);
  formatted[13] = ':';
  auto i = day_time.minutes().count();
  formatted[14] = '0' + (i / 10);
  formatted[15] = '0' + (i % 10);
  formatted[16] = ':';
  auto s = day_time.seconds().count();
  formatted[17] = '0' + (s / 10);
  formatted[18] = '0' + (s % 10);
  formatted[19] = '.';
  uint64_t millis = day_time.subseconds().count();
  if (millis > 999) {
    millis = 999;
  }
  formatted[20] = '0' + (millis / 100);
  formatted[21] = '0' + ((millis % 100) / 10);
  formatted[22] = '0' + (millis % 10);
  formatted[23] = 'Z';

  return AqlValue(&formatted[0], utc ? sizeof(formatted) : sizeof(formatted) - 1);
}

DateSelectionModifier parseDateModifierFlag(VPackSlice flag) {
  if (!flag.isString()) {
    return INVALID;
  }

  std::string flagStr = flag.copyString();
  if (flagStr.empty()) {
    return INVALID;
  }
  TRI_ASSERT(flagStr.size() >= 1);

  basics::StringUtils::tolowerInPlace(flagStr);
  switch (flagStr.front()) {
    case 'y':
      if (flagStr == "years" || flagStr == "year" || flagStr == "y") {
        return YEAR;
      }
      break;
    case 'w':
      if (flagStr == "weeks" || flagStr == "week" || flagStr == "w") {
        return WEEK;
      }
      break;
    case 'm':
      if (flagStr == "months" || flagStr == "month" || flagStr == "m") {
        return MONTH;
      }
      // Can be minute as well
      if (flagStr == "minutes" || flagStr == "minute") {
        return MINUTE;
      }
      // Can be millisecond as well
      if (flagStr == "milliseconds" || flagStr == "millisecond") {
        return MILLI;
      }
      break;
    case 'd':
      if (flagStr == "days" || flagStr == "day" || flagStr == "d") {
        return DAY;
      }
      break;
    case 'h':
      if (flagStr == "hours" || flagStr == "hour" || flagStr == "h") {
        return HOUR;
      }
      break;
    case 's':
      if (flagStr == "seconds" || flagStr == "second" || flagStr == "s") {
        return SECOND;
      }
      break;
    case 'i':
      if (flagStr == "i") {
        return MINUTE;
      }
      break;
    case 'f':
      if (flagStr == "f") {
        return MILLI;
      }
      break;
  }
  // If we get here the flag is invalid
  return INVALID;
}

AqlValue addOrSubtractUnitFromTimestamp(ExpressionContext* expressionContext,
                                        tp_sys_clock_ms const& tp,
                                        VPackSlice durationUnitsSlice, VPackSlice durationType,
                                        char const* AFN, bool isSubtract) {
  bool isInteger = durationUnitsSlice.isInteger();
  double durationUnits = durationUnitsSlice.getNumber<double>();
  std::chrono::duration<double, std::ratio<1l, 1000l>> ms{};
  year_month_day ymd{floor<days>(tp)};
  auto day_time = make_time(tp - sys_days(ymd));

  DateSelectionModifier flag = ::parseDateModifierFlag(durationType);
  double intPart = 0.0;
  if (durationUnits < 0.0) {
    // Make sure duration is always positive. So we flip isSubtract in this
    // case.
    isSubtract = !isSubtract;
    durationUnits *= -1.0;
  }
  TRI_ASSERT(durationUnits >= 0.0);

  // All Fallthroughs intentional. We still have some remainder
  switch (flag) {
    case YEAR:
      durationUnits = std::modf(durationUnits, &intPart);
      if (isSubtract) {
        ymd -= years{static_cast<int64_t>(intPart)};
      } else {
        ymd += years{static_cast<int64_t>(intPart)};
      }
      if (isInteger || durationUnits == 0.0) {
        break;  // We are done
      }
      durationUnits *= 12;
      [[fallthrough]];
    case MONTH:
      durationUnits = std::modf(durationUnits, &intPart);
      if (isSubtract) {
        ymd -= months{static_cast<int64_t>(intPart)};
      } else {
        ymd += months{static_cast<int64_t>(intPart)};
      }
      if (isInteger || durationUnits == 0.0) {
        break;  // We are done
      }
      durationUnits *= 30;  // 1 Month ~= 30 Days
      [[fallthrough]];
    // After this fall through the date may actually a bit off
    case DAY:
      // From here on we do not need leap-day handling
      ms = days{1};
      ms *= durationUnits;
      break;
    case WEEK:
      ms = weeks{1};
      ms *= durationUnits;
      break;
    case HOUR:
      ms = hours{1};
      ms *= durationUnits;
      break;
    case MINUTE:
      ms = minutes{1};
      ms *= durationUnits;
      break;
    case SECOND:
      ms = seconds{1};
      ms *= durationUnits;
      break;
    case MILLI:
      ms = milliseconds{1};
      ms *= durationUnits;
      break;
    default:
      aql::registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return AqlValue(AqlValueHintNull());
  }
  // Here we reconstruct the timepoint again

  tp_sys_clock_ms resTime;
  if (isSubtract) {
    resTime =
        tp_sys_clock_ms{sys_days(ymd) + day_time.to_duration() -
                        std::chrono::duration_cast<duration<int64_t, std::milli>>(ms)};
  } else {
    resTime =
        tp_sys_clock_ms{sys_days(ymd) + day_time.to_duration() +
                        std::chrono::duration_cast<duration<int64_t, std::milli>>(ms)};
  }
  return ::timeAqlValue(expressionContext, AFN, resTime);
}

AqlValue addOrSubtractIsoDurationFromTimestamp(ExpressionContext* expressionContext,
                                               tp_sys_clock_ms const& tp,
                                               arangodb::velocypack::StringRef duration,
                                               char const* AFN, bool isSubtract) {
  year_month_day ymd{floor<days>(tp)};
  auto day_time = make_time(tp - sys_days(ymd));

  std::match_results<char const*> durationParts;
  if (!basics::regexIsoDuration(duration, durationParts)) {
    aql::registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_DATE_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  char const* begin;

  begin = duration.data() + durationParts.position(2);
  int number = NumberUtils::atoi_unchecked<int>(begin, begin + durationParts.length(2));
  if (isSubtract) {
    ymd -= years{number};
  } else {
    ymd += years{number};
  }

  begin = duration.data() + durationParts.position(4);
  number = NumberUtils::atoi_unchecked<int>(begin, begin + durationParts.length(4));
  if (isSubtract) {
    ymd -= months{number};
  } else {
    ymd += months{number};
  }

  milliseconds ms{0};
  begin = duration.data() + durationParts.position(6);
  number = NumberUtils::atoi_unchecked<int>(begin, begin + durationParts.length(6));
  ms += weeks{number};

  begin = duration.data() + durationParts.position(8);
  number = NumberUtils::atoi_unchecked<int>(begin, begin + durationParts.length(8));
  ms += days{number};

  begin = duration.data() + durationParts.position(11);
  number = NumberUtils::atoi_unchecked<int>(begin, begin + durationParts.length(11));
  ms += hours{number};

  begin = duration.data() + durationParts.position(13);
  number = NumberUtils::atoi_unchecked<int>(begin, begin + durationParts.length(13));
  ms += minutes{number};

  begin = duration.data() + durationParts.position(15);
  number = NumberUtils::atoi_unchecked<int>(begin, begin + durationParts.length(15));
  ms += seconds{number};

  // The Milli seconds can be shortened:
  // .1 => 100ms
  // so we append 00 but only take the first 3 digits
  std::size_t matchLength = durationParts.length(17);
  number = 0;
  if (matchLength > 0) {
    if (matchLength > 3) {
      matchLength = 3;
    }
    begin = duration.data() + durationParts.position(17);
    number = NumberUtils::atoi_unchecked<int>(begin, begin + matchLength);
    if (matchLength == 2) {
      number *= 10;
    } else if (matchLength == 1) {
      number *= 100;
    }
  }
  ms += milliseconds{number};

  tp_sys_clock_ms resTime;
  if (isSubtract) {
    resTime = tp_sys_clock_ms{sys_days(ymd) + day_time.to_duration() - ms};
  } else {
    resTime = tp_sys_clock_ms{sys_days(ymd) + day_time.to_duration() + ms};
  }
  return ::timeAqlValue(expressionContext, AFN, resTime);
}

bool parameterToTimePoint(ExpressionContext* expressionContext,
                          VPackFunctionParameters const& parameters,
                          tp_sys_clock_ms& tp, char const* AFN, size_t parameterIndex) {
  AqlValue const& value = extractFunctionParameterValue(parameters, parameterIndex);

  if (value.isNumber()) {
    int64_t v = value.toInt64();
    if (ADB_UNLIKELY(v < -62167219200000 || v > 253402300799999)) {
      // check if value is between "0000-01-01T00:00:00.000Z" and
      // "9999-12-31T23:59:59.999Z" -62167219200000: "0000-01-01T00:00:00.000Z"
      // 253402300799999: "9999-12-31T23:59:59.999Z"
      aql::registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return false;
    }
    tp = tp_sys_clock_ms(milliseconds(v));
    return true;
  }

  if (value.isString()) {
    if (!basics::parseDateTime(value.slice().stringRef(), tp)) {
      aql::registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return false;
    }
    return true;
  }

  registerInvalidArgumentWarning(expressionContext, AFN);
  return false;
}

/// @brief converts a value into a number value
double valueToNumber(VPackSlice const& slice, bool& isValid) {
  if (slice.isNull()) {
    isValid = true;
    return 0.0;
  }
  if (slice.isBoolean()) {
    isValid = true;
    return (slice.getBoolean() ? 1.0 : 0.0);
  }
  if (slice.isNumber()) {
    isValid = true;
    return slice.getNumericValue<double>();
  }
  if (slice.isString()) {
    std::string const str = slice.copyString();
    try {
      if (str.empty()) {
        isValid = true;
        return 0.0;
      }
      size_t behind = 0;
      double value = std::stod(str, &behind);
      while (behind < str.size()) {
        char c = str[behind];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f') {
          isValid = false;
          return 0.0;
        }
        ++behind;
      }
      isValid = true;
      return value;
    } catch (...) {
      size_t behind = 0;
      while (behind < str.size()) {
        char c = str[behind];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f') {
          isValid = false;
          return 0.0;
        }
        ++behind;
      }
      // A string only containing whitespae-characters is valid and should
      // return 0.0
      // It throws in std::stod
      isValid = true;
      return 0.0;
    }
  }
  if (slice.isArray()) {
    VPackValueLength const n = slice.length();
    if (n == 0) {
      isValid = true;
      return 0.0;
    }
    if (n == 1) {
      return ::valueToNumber(slice.at(0), isValid);
    }
  }

  // All other values are invalid
  isValid = false;
  return 0.0;
}

/// @brief extract a boolean parameter from an array
bool getBooleanParameter(transaction::Methods* trx, VPackFunctionParameters const& parameters,
                         size_t startParameter, bool defaultValue) {
  size_t const n = parameters.size();

  if (startParameter >= n) {
    return defaultValue;
  }

  return parameters[startParameter].toBoolean();
}

/// @brief extra a collection name from an AqlValue
std::string extractCollectionName(transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters,
                                  size_t position) {
  AqlValue const& value = extractFunctionParameterValue(parameters, position);

  std::string identifier;

  if (value.isString()) {
    // already a string
    identifier = value.slice().copyString();
  } else {
    AqlValueMaterializer materializer(trx);
    VPackSlice slice = materializer.slice(value, true);
    VPackSlice id = slice;

    if (slice.isObject()) {
      id = slice.get(StaticStrings::IdString);
    }
    if (id.isString()) {
      identifier = id.copyString();
    } else if (id.isCustom()) {
      identifier = trx->extractIdString(slice);
    }
  }

  if (!identifier.empty()) {
    size_t pos = identifier.find('/');

    if (pos != std::string::npos) {
      // this is superior to  identifier.substr(0, pos)
      identifier.resize(pos);
    }

    return identifier;
  }

  return StaticStrings::Empty;
}

/// @brief extract attribute names from the arguments
void extractKeys(std::unordered_set<std::string>& names, ExpressionContext* expressionContext,
                 transaction::Methods* trx, VPackFunctionParameters const& parameters,
                 size_t startParameter, char const* functionName) {
  size_t const n = parameters.size();

  for (size_t i = startParameter; i < n; ++i) {
    AqlValue const& param = extractFunctionParameterValue(parameters, i);

    if (param.isString()) {
      names.emplace(param.slice().copyString());
    } else if (param.isNumber()) {
      double number = param.toDouble();

      if (std::isnan(number) || number == HUGE_VAL || number == -HUGE_VAL) {
        names.emplace("null");
      } else {
        char buffer[24];
        int length = fpconv_dtoa(number, &buffer[0]);
        names.emplace(&buffer[0], static_cast<size_t>(length));
      }
    } else if (param.isArray()) {
      AqlValueMaterializer materializer(trx);
      VPackSlice s = materializer.slice(param, false);

      for (VPackSlice v : VPackArrayIterator(s)) {
        if (v.isString()) {
          names.emplace(v.copyString());
        } else {
          registerInvalidArgumentWarning(expressionContext, functionName);
        }
      }
    }
  }
}

/// @brief append the VelocyPack value to a string buffer
///        Note: Backwards compatibility. Is different than Slice.toJson()
void appendAsString(transaction::Methods* trx,
                    arangodb::basics::VPackStringBufferAdapter& buffer,
                    AqlValue const& value) {
  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  Functions::Stringify(trx, buffer, slice);
}

/// @brief Checks if the given list contains the element
bool listContainsElement(transaction::Methods* trx, VPackOptions const* options,
                         AqlValue const& list, AqlValue const& testee, size_t& index) {
  TRI_ASSERT(list.isArray());
  AqlValueMaterializer materializer(options);
  VPackSlice slice = materializer.slice(list, false);

  AqlValueMaterializer testeeMaterializer(trx);
  VPackSlice testeeSlice = testeeMaterializer.slice(testee, false);

  VPackArrayIterator it(slice);
  while (it.valid()) {
    if (arangodb::basics::VelocyPackHelper::equal(testeeSlice, it.value(), false, options)) {
      index = static_cast<size_t>(it.index());
      return true;
    }
    it.next();
  }
  return false;
}

/// @brief Checks if the given list contains the element
/// DEPRECATED
bool listContainsElement(VPackOptions const* options, VPackSlice const& list,
                         VPackSlice const& testee, size_t& index) {
  TRI_ASSERT(list.isArray());
  for (size_t i = 0; i < static_cast<size_t>(list.length()); ++i) {
    if (arangodb::basics::VelocyPackHelper::equal(testee, list.at(i), false, options)) {
      index = i;
      return true;
    }
  }
  return false;
}

bool listContainsElement(VPackOptions const* options, VPackSlice const& list,
                         VPackSlice const& testee) {
  size_t unused;
  return ::listContainsElement(options, list, testee, unused);
}

/// @brief Computes the Variance of the given list.
///        If successful value will contain the variance and count
///        will contain the number of elements.
///        If not successful value and count contain garbage.
bool variance(transaction::Methods* trx, AqlValue const& values, double& value, size_t& count) {
  TRI_ASSERT(values.isArray());
  value = 0.0;
  count = 0;
  bool unused = false;
  double mean = 0.0;

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(values, false);

  for (VPackSlice element : VPackArrayIterator(slice)) {
    if (!element.isNull()) {
      if (!element.isNumber()) {
        return false;
      }
      double current = ::valueToNumber(element, unused);
      count++;
      double delta = current - mean;
      mean += delta / count;
      value += delta * (current - mean);
    }
  }
  return true;
}

/// @brief Sorts the given list of Numbers in ASC order.
///        Removes all null entries.
///        Returns false if the list contains non-number values.
bool sortNumberList(transaction::Methods* trx, AqlValue const& values,
                    std::vector<double>& result) {
  TRI_ASSERT(values.isArray());
  TRI_ASSERT(result.empty());
  bool unused;
  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(values, false);

  VPackArrayIterator it(slice);
  result.reserve(it.size());
  for (auto const& element : it) {
    if (!element.isNull()) {
      if (!element.isNumber()) {
        return false;
      }
      result.emplace_back(::valueToNumber(element, unused));
    }
  }
  std::sort(result.begin(), result.end());
  return true;
}

/// @brief Helper function to unset or keep all given names in the value.
///        Recursively iterates over sub-object and unsets or keeps their values
///        as well
void unsetOrKeep(transaction::Methods* trx, VPackSlice const& value,
                 std::unordered_set<std::string> const& names,
                 bool unset,  // true means unset, false means keep
                 bool recursive, VPackBuilder& result) {
  TRI_ASSERT(value.isObject());
  VPackObjectBuilder b(&result);  // Close the object after this function
  for (auto const& entry : VPackObjectIterator(value, false)) {
    TRI_ASSERT(entry.key.isString());
    std::string key = entry.key.copyString();
    if ((names.find(key) == names.end()) == unset) {
      // not found and unset or found and keep
      if (recursive && entry.value.isObject()) {
        result.add(entry.key);  // Add the key
        ::unsetOrKeep(trx, entry.value, names, unset, recursive,
                      result);  // Adds the object
      } else {
        if (entry.value.isCustom()) {
          result.add(key, VPackValue(trx->extractIdString(value)));
        } else {
          result.add(key, entry.value);
        }
      }
    }
  }
}

/// @brief Helper function to get a document by it's identifier
///        Lazy Locks the collection if necessary.
void getDocumentByIdentifier(transaction::Methods* trx, std::string& collectionName,
                             std::string const& identifier, bool ignoreError,
                             VPackBuilder& result) {
  transaction::BuilderLeaser searchBuilder(trx);

  size_t pos = identifier.find('/');
  if (pos == std::string::npos) {
    searchBuilder->add(VPackValue(identifier));
  } else {
    if (collectionName.empty()) {
      char const* p = identifier.data() + pos + 1;
      size_t l = identifier.size() - pos - 1;
      searchBuilder->add(VPackValuePair(p, l, VPackValueType::String));
      collectionName = identifier.substr(0, pos);
    } else if (identifier.compare(0, pos, collectionName) != 0) {
      // Requesting an _id that cannot be stored in this collection
      if (ignoreError) {
        return;
      }
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST);
    } else {
      char const* p = identifier.data() + pos + 1;
      size_t l = identifier.size() - pos - 1;
      searchBuilder->add(VPackValuePair(p, l, VPackValueType::String));
    }
  }

  Result res;
  try {
    res = trx->documentFastPath(collectionName, nullptr, searchBuilder->slice(), result);
  } catch (arangodb::basics::Exception const& ex) {
    res.reset(ex.code());
  }

  if (!res.ok()) {
    if (ignoreError) {
      if (res.errorNumber() == TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND ||
          res.errorNumber() == TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND ||
          res.errorNumber() == TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST) {
        return;
      }
    }
    if (res.is(TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION)) {
      // special error message to indicate which collection was undeclared
      THROW_ARANGO_EXCEPTION_MESSAGE(res.errorNumber(),
                                     res.errorMessage() + ": " + collectionName +
                                         " [" + AccessMode::typeString(AccessMode::Type::READ) +
                                         "]");
    }
    THROW_ARANGO_EXCEPTION(res);
  }
}

/// @brief Helper function to merge given parameters
///        Works for an array of objects as first parameter or arbitrary many
///        object parameters
AqlValue mergeParameters(ExpressionContext* expressionContext, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters,
                         char const* funcName, bool recursive) {
  size_t const n = parameters.size();

  if (n == 0) {
    return AqlValue(AqlValueHintEmptyObject());
  }

  // use the first argument as the preliminary result
  AqlValue const& initial = extractFunctionParameterValue(parameters, 0);
  AqlValueMaterializer materializer(trx);
  VPackSlice initialSlice = materializer.slice(initial, true);

  VPackBuilder builder;

  if (initial.isArray() && n == 1) {
    // special case: a single array parameter
    // Create an empty document as start point
    builder.openObject();
    builder.close();
    // merge in all other arguments
    for (VPackSlice it : VPackArrayIterator(initialSlice)) {
      if (!it.isObject()) {
        registerInvalidArgumentWarning(expressionContext, funcName);
        return AqlValue(AqlValueHintNull());
      }
      builder = arangodb::basics::VelocyPackHelper::merge(builder.slice(), it,
                                                          false, recursive);
    }
    return AqlValue(builder.slice(), builder.size());
  }

  if (!initial.isObject()) {
    registerInvalidArgumentWarning(expressionContext, funcName);
    return AqlValue(AqlValueHintNull());
  }

  // merge in all other arguments
  for (size_t i = 1; i < n; ++i) {
    AqlValue const& param = extractFunctionParameterValue(parameters, i);

    if (!param.isObject()) {
      registerInvalidArgumentWarning(expressionContext, funcName);
      return AqlValue(AqlValueHintNull());
    }

    AqlValueMaterializer materializer(trx);
    VPackSlice slice = materializer.slice(param, false);

    builder = arangodb::basics::VelocyPackHelper::merge(initialSlice, slice, false, recursive);
    initialSlice = builder.slice();
  }
  if (n == 1) {
    // only one parameter. now add original document
    builder.add(initialSlice);
  }
  return AqlValue(builder.slice(), builder.size());
}

/// @brief internal recursive flatten helper
void flattenList(VPackSlice const& array, size_t maxDepth, size_t curDepth,
                 VPackBuilder& result) {
  TRI_ASSERT(result.isOpenArray());
  for (auto const& tmp : VPackArrayIterator(array)) {
    if (tmp.isArray() && curDepth < maxDepth) {
      ::flattenList(tmp, maxDepth, curDepth + 1, result);
    } else {
      // Copy the content of tmp into the result
      result.add(tmp);
    }
  }
}

/**
 * @brief Parses 1 or 3-7 input parameters and creates a Date object out of it.
 *        This object can either be a timestamp in milliseconds or an ISO_8601
 * DATE
 *
 * @param expressionContext The AQL expression context
 * @param trx The used transaction
 * @param parameters list of parameters, only 1 or 3-7 are allowed
 * @param asTimestamp If it should return a timestamp (true) or ISO_DATE (false)
 *
 * @return Returns a timestamp if asTimestamp is true, an ISO_DATE otherwise
 */
AqlValue dateFromParameters(ExpressionContext* expressionContext,
                            VPackFunctionParameters const& parameters,
                            char const* AFN, bool asTimestamp) {
  tp_sys_clock_ms tp;
  duration<int64_t, std::milli> time;

  if (parameters.size() == 1) {
    if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
      return AqlValue(AqlValueHintNull());
    }
    time = tp.time_since_epoch();
  } else {
    if (parameters.size() < 3 || parameters.size() > 7) {
      // YMD is a must
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    for (uint8_t i = 0; i < parameters.size(); i++) {
      AqlValue const& value = extractFunctionParameterValue(parameters, i);

      // All Parameters have to be a number or a string
      if (!value.isNumber() && !value.isString()) {
        registerInvalidArgumentWarning(expressionContext, AFN);
        return AqlValue(AqlValueHintNull());
      }
    }

    years y{extractFunctionParameterValue(parameters, 0).toInt64()};
    months m{extractFunctionParameterValue(parameters, 1).toInt64()};
    days d{extractFunctionParameterValue(parameters, 2).toInt64()};

    if ((y < years{0} || y > years{9999}) || (m < months{0}) || (d < days{0})) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return AqlValue(AqlValueHintNull());
    }
    year_month_day ymd = year{y.count()} / m.count() / d.count();

    // Parse the time
    hours h(0);
    minutes min(0);
    seconds s(0);
    milliseconds ms(0);

    if (parameters.size() >= 4) {
      h = hours((extractFunctionParameterValue(parameters, 3).toInt64()));
    }
    if (parameters.size() >= 5) {
      min = minutes((extractFunctionParameterValue(parameters, 4).toInt64()));
    }
    if (parameters.size() >= 6) {
      s = seconds((extractFunctionParameterValue(parameters, 5).toInt64()));
    }
    if (parameters.size() == 7) {
      int64_t v = extractFunctionParameterValue(parameters, 6).toInt64();
      if (v > 999) {
        v = 999;
      }
      ms = milliseconds(v);
    }

    if ((h < hours{0}) || (min < minutes{0}) || (s < seconds{0}) ||
        (ms < milliseconds{0})) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return AqlValue(AqlValueHintNull());
    }

    time = sys_days(ymd).time_since_epoch();
    time += h;
    time += min;
    time += s;
    time += ms;
    tp = tp_sys_clock_ms(time);
  }

  if (asTimestamp) {
    return AqlValue(AqlValueHintInt(time.count()));
  }
  return ::timeAqlValue(expressionContext, AFN, tp);
}

AqlValue callApplyBackend(ExpressionContext* expressionContext, transaction::Methods* trx,
                          char const* AFN, AqlValue const& invokeFN,
                          VPackFunctionParameters const& invokeParams) {
  std::string ucInvokeFN;
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, invokeFN);

  icu::UnicodeString unicodeStr(buffer->c_str(), static_cast<int32_t>(buffer->length()));
  unicodeStr.toUpper(nullptr);
  unicodeStr.toUTF8String(ucInvokeFN);

  arangodb::aql::Function const* func = nullptr;
  if (ucInvokeFN.find("::") == std::string::npos) {
    // built-in C++ function
    func = AqlFunctionFeature::getFunctionByName(ucInvokeFN);
    if (func->implementation != nullptr) {
      std::pair<size_t, size_t> numExpectedArguments = func->numArguments();

      if (invokeParams.size() < numExpectedArguments.first ||
          invokeParams.size() > numExpectedArguments.second) {
        THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH,
                                      ucInvokeFN.c_str(),
                                      static_cast<int>(numExpectedArguments.first),
                                      static_cast<int>(numExpectedArguments.second));
      }

      return func->implementation(expressionContext, trx, invokeParams);
    }
  }

  // JavaScript function (this includes user-defined functions)
  {
    ISOLATE;
    TRI_V8_CURRENT_GLOBALS_AND_SCOPE;
    auto context = TRI_IGETC;


    auto old = v8g->_expressionContext;
    v8g->_expressionContext = expressionContext;
    TRI_DEFER(v8g->_expressionContext = old);

    VPackOptions const* options = trx->transactionContext()->getVPackOptions();
    std::string jsName;
    int const n = static_cast<int>(invokeParams.size());
    int const callArgs = (func == nullptr ? 3 : n);
    auto args = std::make_unique<v8::Handle<v8::Value>[]>(callArgs);

    if (func == nullptr) {
      // a call to a user-defined function
      jsName = "FCALL_USER";

      // function name
      args[0] = TRI_V8_STD_STRING(isolate, ucInvokeFN);
      // call parameters
      v8::Handle<v8::Array> params = v8::Array::New(isolate, static_cast<int>(n));

      for (int i = 0; i < n; ++i) {
        params
            ->Set(context, static_cast<uint32_t>(i), invokeParams[i].toV8(isolate, options))
            .FromMaybe(true);
      }
      args[1] = params;
      args[2] = TRI_V8_ASCII_STRING(isolate, AFN);
    } else {
      // a call to a built-in V8 function
      jsName = "AQL_" + func->name;
      for (int i = 0; i < n; ++i) {
        args[i] = invokeParams[i].toV8(isolate, options);
      }
    }

    bool dummy;
    return Expression::invokeV8Function(expressionContext, jsName, ucInvokeFN,
                                        AFN, false, callArgs, args.get(), dummy);
  }
}

AqlValue geoContainsIntersect(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters,
                              char const* func, bool contains) {
  AqlValue const& p1 = extractFunctionParameterValue(parameters, 0);
  AqlValue const& p2 = extractFunctionParameterValue(parameters, 1);

  if (!p1.isObject()) {
    registerWarning(expressionContext, func,
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "Expecting GeoJSON object"));
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer mat1(trx);
  geo::ShapeContainer outer, inner;
  Result res = geo::geojson::parseRegion(mat1.slice(p1, true), outer);
  if (res.fail()) {
    registerWarning(expressionContext, func, res);
    return AqlValue(AqlValueHintNull());
  }
  if (contains && !outer.isAreaType()) {
    registerWarning(
        expressionContext, func,
        Result(
            TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
            "Only Polygon and MultiPolygon types are valid as first argument"));
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer mat2(trx);
  if (p2.isArray() && p2.length() >= 2) {
    res = inner.parseCoordinates(mat2.slice(p2, true), /*geoJson*/ true);
  } else if (p2.isObject()) {
    res = geo::geojson::parseRegion(mat2.slice(p2, true), inner);
  } else {
    res.reset(TRI_ERROR_BAD_PARAMETER,
              "Second arg requires coordinate pair or GeoJSON");
  }
  if (res.fail()) {
    registerWarning(expressionContext, func, res);
    return AqlValue(AqlValueHintNull());
  }

  bool result = contains ? outer.contains(&inner) : outer.intersects(&inner);
  return AqlValue(AqlValueHintBool(result));
}

static Result parseGeoPolygon(VPackSlice polygon, VPackBuilder& b) {
  // check if nested or not
  bool unnested = false;
  for (VPackSlice v : VPackArrayIterator(polygon)) {
    if (v.isArray() && v.length() == 2) {
      unnested = true;
    }
  }

  if (unnested) {
    b.openArray();
  }

  if (!polygon.isArray()) {
    return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                  "Polygon needs to be an array of positions.");
  }

  for (VPackSlice v : VPackArrayIterator(polygon)) {
    if (v.isArray() && v.length() > 2) {
      b.openArray();
      for (auto const& coord : VPackArrayIterator(v)) {
        if (coord.isNumber()) {
          b.add(VPackValue(coord.getNumber<double>()));
        } else if (coord.isArray()) {
          if (coord.length() < 2) {
            return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                          "a Position needs at least two numeric values");
          } else {
            b.openArray();
            for (auto const& innercord : VPackArrayIterator(coord)) {
              if (innercord.isNumber()) {
                b.add(VPackValue(innercord.getNumber<double>()));  // TODO
              } else if (innercord.isArray() && innercord.length() == 2) {
                if (innercord.at(0).isNumber() && innercord.at(1).isNumber()) {
                  b.openArray();
                  b.add(VPackValue(innercord.at(0).getNumber<double>()));
                  b.add(VPackValue(innercord.at(1).getNumber<double>()));
                  b.close();
                } else {
                  return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                                "coordinate is not a number");
                }
              } else {
                return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                              "not an array describing a position");
              }
            }
            b.close();
          }
        } else {
          return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                        "not an array containing positions");
        }
      }
      b.close();
    } else if (v.isArray() && v.length() == 2) {
      if (polygon.length() > 2) {
        b.openArray();
        for (auto const& innercord : VPackArrayIterator(v)) {
          if (innercord.isNumber()) {
            b.add(VPackValue(innercord.getNumber<double>()));
          } else if (innercord.isArray() && innercord.length() == 2) {
            if (innercord.at(0).isNumber() && innercord.at(1).isNumber()) {
              b.openArray();
              b.add(VPackValue(innercord.at(0).getNumber<double>()));
              b.add(VPackValue(innercord.at(1).getNumber<double>()));
              b.close();
            } else {
              return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                            "coordinate is not a number");
            }
          } else {
            return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                          "not a numeric value");
          }
        }
        b.close();
      } else {
        return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                      "a Polygon needs at least three positions");
      }
    } else {
      return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                    "not an array containing positions");
    }
  }

  if (unnested) {
    b.close();
  }

  return {TRI_ERROR_NO_ERROR};
}

Result parseShape(transaction::Methods& trx,
                  AqlValue const& value,
                  geo::ShapeContainer& shape) {
  AqlValueMaterializer mat(&trx);

  if (value.isArray() && value.length() >= 2) {
    return shape.parseCoordinates(mat.slice(value, true), /*geoJson*/ true);
  } else if (value.isObject()) {
    return geo::geojson::parseRegion(mat.slice(value, true), shape);
  } else {
    return {TRI_ERROR_BAD_PARAMETER, "Requires coordinate pair or GeoJSON"};
  }
}

}  // namespace

namespace arangodb {
namespace aql {

/// @brief register warning
void registerWarning(ExpressionContext* expressionContext,
                     char const* functionName, Result const& rr) {
  std::string msg = "in function '";
  msg.append(functionName);
  msg.append("()': ");
  msg.append(rr.errorMessage());
  expressionContext->registerWarning(rr.errorNumber(), msg.c_str());
}

/// @brief register warning
void registerWarning(ExpressionContext* expressionContext,
                     char const* functionName, int code) {
  if (code != TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH &&
      code != TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH) {
    registerWarning(expressionContext, functionName, Result(code));
    return;
  }

  std::string msg = arangodb::basics::Exception::FillExceptionString(code, functionName);
  expressionContext->registerWarning(code, msg.c_str());
}

void registerError(ExpressionContext* expressionContext, char const* functionName, int code) {
  std::string msg;

  if (code == TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH) {
    msg = arangodb::aql::QueryWarnings::buildFormattedString(code, functionName);
  } else {
    msg.append("in function '");
    msg.append(functionName);
    msg.append("()': ");
    msg.append(TRI_errno_string(code));
  }

  expressionContext->registerError(code, msg.c_str());
}

/// @brief register usage of an invalid function argument
void registerInvalidArgumentWarning(ExpressionContext* expressionContext,
                                    char const* functionName) {
  registerWarning(expressionContext, functionName,
                  TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
}

}  // namespace aql
}  // namespace arangodb

/// @brief append the VelocyPack value to a string buffer
///        Note: Backwards compatibility. Is different than Slice.toJson()
void Functions::Stringify(transaction::Methods* trx,
                          arangodb::basics::VPackStringBufferAdapter& buffer,
                          VPackSlice const& slice) {
  if (slice.isNull()) {
    // null is the empty string
    return;
  }

  if (slice.isString()) {
    // dumping adds additional ''
    VPackValueLength length;
    char const* p = slice.getStringUnchecked(length);
    buffer.append(p, length);
    return;
  }

  VPackOptions* options = trx->transactionContextPtr()->getVPackOptions();
  VPackOptions adjustedOptions = *options;
  adjustedOptions.escapeUnicode = false;
  adjustedOptions.escapeForwardSlashes = false;
  VPackDumper dumper(&buffer, &adjustedOptions);
  dumper.dump(slice);
}

/// @brief function IS_NULL
AqlValue Functions::IsNull(ExpressionContext*, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isNull(true)));
}

/// @brief function IS_BOOL
AqlValue Functions::IsBool(ExpressionContext*, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isBoolean()));
}

/// @brief function IS_NUMBER
AqlValue Functions::IsNumber(ExpressionContext*, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isNumber()));
}

/// @brief function IS_STRING
AqlValue Functions::IsString(ExpressionContext*, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isString()));
}

/// @brief function IS_ARRAY
AqlValue Functions::IsArray(ExpressionContext*, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isArray()));
}

/// @brief function IS_OBJECT
AqlValue Functions::IsObject(ExpressionContext*, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isObject()));
}

/// @brief function TYPENAME
AqlValue Functions::Typename(ExpressionContext*, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  char const* type = value.getTypeString();

  return AqlValue(TRI_CHAR_LENGTH_PAIR(type));
}

/// @brief function TO_NUMBER
AqlValue Functions::ToNumber(ExpressionContext*, transaction::Methods*,
                             VPackFunctionParameters const& parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  bool failed;
  double value = a.toDouble(failed);

  if (failed) {
    return AqlValue(AqlValueHintZero());
  }

  return AqlValue(AqlValueHintDouble(value));
}

/// @brief function TO_STRING
AqlValue Functions::ToString(ExpressionContext*, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);
  return AqlValue(buffer->begin(), buffer->length());
}

/// @brief function TO_BASE64
AqlValue Functions::ToBase64(ExpressionContext*, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  std::string encoded =
      basics::StringUtils::encodeBase64(buffer->begin(), buffer->length());

  return AqlValue(encoded);
}

/// @brief function TO_HEX
AqlValue Functions::ToHex(ExpressionContext*, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  std::string encoded =
      basics::StringUtils::encodeHex(buffer->begin(), buffer->length());

  return AqlValue(encoded);
}

/// @brief function ENCODE_URI_COMPONENT
AqlValue Functions::EncodeURIComponent(ExpressionContext*, transaction::Methods* trx,
                                       VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  std::string encoded =
      basics::StringUtils::encodeURIComponent(buffer->begin(), buffer->length());

  return AqlValue(encoded);
}

/// @brief function UUID
AqlValue Functions::Uuid(ExpressionContext*, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  boost::uuids::uuid uuid;
  {
    // must protect mutex generation from races
    MUTEX_LOCKER(mutexLocker, ::uuidMutex);
    uuid = boost::uuids::random_generator()();
  }

  return AqlValue(boost::uuids::to_string(uuid));
}

/// @brief function SOUNDEX
AqlValue Functions::Soundex(ExpressionContext*, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  std::string encoded = basics::StringUtils::soundex(basics::StringUtils::trim(
      basics::StringUtils::tolower(std::string(buffer->begin(), buffer->length()))));

  return AqlValue(encoded);
}

/// @brief function LEVENSHTEIN_DISTANCE
AqlValue Functions::LevenshteinDistance(ExpressionContext*, transaction::Methods* trx,
                                        VPackFunctionParameters const& parameters) {
  AqlValue const& value1 = extractFunctionParameterValue(parameters, 0);
  AqlValue const& value2 = extractFunctionParameterValue(parameters, 1);

  // we use one buffer to stringify both arguments
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  // stringify argument 1
  ::appendAsString(trx, adapter, value1);

  // note split position
  size_t const split = buffer->length();

  // stringify argument 2
  ::appendAsString(trx, adapter, value2);

  int encoded = basics::StringUtils::levenshteinDistance(buffer->begin(), split,
                                                         buffer->begin() + split,
                                                         buffer->length() - split);

  return AqlValue(AqlValueHintInt(encoded));
}

namespace {
template <bool search_semantics>
AqlValue NgramSimilarityHelper(char const* AFN, ExpressionContext* ctx,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& args) {
  TRI_ASSERT(ctx);
  if (args.size() < 3) {
    registerWarning(ctx, AFN,
                    arangodb::Result{TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH,
                                     "Minimum 3 arguments are expected."});
    return AqlValue(AqlValueHintNull());
  }

  auto const& attribute = extractFunctionParameterValue(args, 0);
  if (ADB_UNLIKELY(!attribute.isString())) {
    arangodb::aql::registerInvalidArgumentWarning(ctx, AFN);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }
  auto const attributeValue = arangodb::iresearch::getStringRef(attribute.slice());

  auto const& target = extractFunctionParameterValue(args, 1);
  if (ADB_UNLIKELY(!target.isString())) {
    arangodb::aql::registerInvalidArgumentWarning(ctx, AFN);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }
  auto const targetValue = arangodb::iresearch::getStringRef(target.slice());

  auto const& ngramSize = extractFunctionParameterValue(args, 2);
  if (ADB_UNLIKELY(!ngramSize.isNumber())) {
    arangodb::aql::registerInvalidArgumentWarning(ctx, AFN);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }
  auto const ngramSizeValue = ngramSize.toInt64();

  if (ADB_UNLIKELY(ngramSizeValue < 1)) {
    arangodb::aql::registerWarning(
        ctx, AFN,
        arangodb::Result{TRI_ERROR_BAD_PARAMETER,
                         "Invalid ngram size. Should be 1 or greater"});
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }

  auto utf32Attribute = basics::StringUtils::characterCodes(attributeValue.c_str(),
                                                            attributeValue.size());
  auto utf32Target =
      basics::StringUtils::characterCodes(targetValue.c_str(), targetValue.size());

  auto const similarity = irs::ngram_similarity<uint32_t, search_semantics>(
      utf32Target.data(), utf32Target.size(), utf32Attribute.data(),
      utf32Attribute.size(), ngramSizeValue);
  return AqlValue(AqlValueHintDouble(similarity));
}
}  // namespace

/// Executes NGRAM_SIMILARITY based on binary ngram similarity
AqlValue Functions::NgramSimilarity(ExpressionContext* ctx, transaction::Methods* trx,
                                    VPackFunctionParameters const& args) {
  static char const* AFN = "NGRAM_SIMILARITY";
  return NgramSimilarityHelper<true>(AFN, ctx, trx, args);
}

/// Executes NGRAM_POSITIONAL_SIMILARITY based on positional ngram similarity
AqlValue Functions::NgramPositionalSimilarity(ExpressionContext* ctx,
                                              transaction::Methods* trx,
                                              VPackFunctionParameters const& args) {
  static char const* AFN = "NGRAM_POSITIONAL_SIMILARITY";
  return NgramSimilarityHelper<false>(AFN, ctx, trx, args);
}

/// Executes NGRAM_MATCH based on binary ngram similarity
AqlValue Functions::NgramMatch(ExpressionContext* ctx, transaction::Methods* trx,
                               VPackFunctionParameters const& args) {
  TRI_ASSERT(ctx);
  static char const* AFN = "NGRAM_MATCH";

  auto const argc = args.size();

  if (argc < 3) {  // for const evaluation we need analyzer to be set explicitly (we can`t access filter context)
                   // but we can`t set analyzer as mandatory in function AQL signature - this will break SEARCH
    registerWarning(ctx, AFN,
                    arangodb::Result{TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH,
                                     "Minimum 3 arguments are expected."});
    return AqlValue(AqlValueHintNull());
  }

  auto const& attribute = extractFunctionParameterValue(args, 0);
  if (ADB_UNLIKELY(!attribute.isString())) {
    arangodb::aql::registerInvalidArgumentWarning(ctx, AFN);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }
  auto const attributeValue = iresearch::getStringRef(attribute.slice());

  auto const& target = extractFunctionParameterValue(args, 1);
  if (ADB_UNLIKELY(!target.isString())) {
    arangodb::aql::registerInvalidArgumentWarning(ctx, AFN);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }
  auto const targetValue = iresearch::getStringRef(target.slice());

  auto threshold = arangodb::iresearch::FilterConstants::DefaultNgramMatchThreshold;
  size_t analyzerPosition = 2;
  if (argc > 3) {  // 4 args given. 3rd is threshold
    auto const& thresholdArg = extractFunctionParameterValue(args, 2);
    analyzerPosition = 3;
    if (ADB_UNLIKELY(!thresholdArg.isNumber())) {
      arangodb::aql::registerInvalidArgumentWarning(ctx, AFN);
      return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
    }
    threshold = thresholdArg.toDouble();
    if (threshold <= 0 || threshold > 1) {
      arangodb::aql::registerWarning(
          ctx, AFN,
          arangodb::Result{TRI_ERROR_BAD_PARAMETER,
                           "Threshold must be between 0 and 1"});
    }
  }

  auto const& analyzerArg = extractFunctionParameterValue(args, analyzerPosition);
  if (ADB_UNLIKELY(!analyzerArg.isString())) {
    arangodb::aql::registerInvalidArgumentWarning(ctx, AFN);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }
  if (ADB_UNLIKELY(nullptr == trx)) {
    arangodb::aql::registerWarning(ctx, AFN, TRI_ERROR_INTERNAL);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }
  auto const analyzerId = arangodb::iresearch::getStringRef(analyzerArg.slice());
  auto& server = trx->vocbase().server();
  if (!server.hasFeature<iresearch::IResearchAnalyzerFeature>()) {
    arangodb::aql::registerWarning(ctx, AFN, TRI_ERROR_INTERNAL);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }
  auto& analyzerFeature = server.getFeature<iresearch::IResearchAnalyzerFeature>();
  auto analyzer = analyzerFeature.get(analyzerId, trx->vocbase(),
                                      trx->state()->analyzersRevision());
  if (!analyzer) {
    arangodb::aql::registerWarning(
        ctx, AFN,
        arangodb::Result{TRI_ERROR_BAD_PARAMETER,
                         "Unable to load requested analyzer"});
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }

  auto analyzerImpl = analyzer->get();
  TRI_ASSERT(analyzerImpl);
  irs::term_attribute const* token = irs::get<irs::term_attribute>(*analyzerImpl);
  TRI_ASSERT(token);

  std::vector<irs::bstring> attrNgrams;
  analyzerImpl->reset(attributeValue);
  while (analyzerImpl->next()) {
    attrNgrams.push_back(token->value);
  }

  std::vector<irs::bstring> targetNgrams;
  analyzerImpl->reset(targetValue);
  while (analyzerImpl->next()) {
    targetNgrams.push_back(token->value);
  }

  // consider only non empty ngrams sets. As no ngrams emitted - means no data in index = no match
  if (!targetNgrams.empty() && !attrNgrams.empty()) {
    size_t thresholdMatches = (size_t)std::ceil((float_t)targetNgrams.size() * threshold);
    size_t d = 0;  // will store upper-left cell value for current cache row
    std::vector<size_t> cache(attrNgrams.size() + 1, 0);
    for (auto const& targetNgram : targetNgrams) {
      size_t s_ngram_idx = 1;
      for (; s_ngram_idx <= attrNgrams.size(); ++s_ngram_idx) {
        size_t curMatches = d + (size_t)(attrNgrams[s_ngram_idx - 1] == targetNgram);
        if (curMatches >= thresholdMatches) {
          return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintBool{true}};
        }
        auto tmp = cache[s_ngram_idx];
        cache[s_ngram_idx] =
            std::max(std::max(cache[s_ngram_idx - 1], cache[s_ngram_idx]), curMatches);
        d = tmp;
      }
    }
  }
  return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintBool{false}};
}

/// Executes LEVENSHTEIN_MATCH
AqlValue Functions::LevenshteinMatch(ExpressionContext* ctx, transaction::Methods* trx,
                                     VPackFunctionParameters const& args) {
  static char const* AFN = "LEVENSHTEIN_MATCH";

  auto const& maxDistance = extractFunctionParameterValue(args, 2);

  if (ADB_UNLIKELY(!maxDistance.isNumber())) {
    arangodb::aql::registerInvalidArgumentWarning(ctx, AFN);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }

  bool withTranspositionsValue = true;
  int64_t maxDistanceValue = maxDistance.toInt64();

  if (args.size() > 3) {
    auto const& withTranspositions = extractFunctionParameterValue(args, 3);

    if (ADB_UNLIKELY(!withTranspositions.isBoolean())) {
      registerInvalidArgumentWarning(ctx, AFN);
      return AqlValue{AqlValueHintNull{}};
    }

    withTranspositionsValue = withTranspositions.toBoolean();
  }

  if (maxDistanceValue < 0 ||
      (!withTranspositionsValue &&
       maxDistanceValue > arangodb::iresearch::MAX_LEVENSHTEIN_DISTANCE)) {
    registerInvalidArgumentWarning(ctx, AFN);
    return AqlValue{AqlValueHintNull{}};
  }

  if (withTranspositionsValue && maxDistanceValue > arangodb::iresearch::MAX_DAMERAU_LEVENSHTEIN_DISTANCE) {
    // fallback to LEVENSHTEIN_DISTANCE
    auto const dist = Functions::LevenshteinDistance(ctx, trx, args);
    TRI_ASSERT(dist.isNumber());

    return AqlValue{AqlValueHintBool{dist.toInt64() <= maxDistanceValue}};
  }

  size_t const unsignedMaxDistanceValue = static_cast<size_t>(maxDistanceValue);

  auto& description =
      arangodb::iresearch::getParametricDescription(static_cast<irs::byte_type>(unsignedMaxDistanceValue),
                                                    withTranspositionsValue);

  if (ADB_UNLIKELY(!description)) {
    registerInvalidArgumentWarning(ctx, AFN);
    return AqlValue{AqlValueHintNull{}};
  }

  auto const& lhs = extractFunctionParameterValue(args, 0);
  auto const lhsValue = lhs.isString() ? iresearch::getBytesRef(lhs.slice())
                                       : irs::bytes_ref::EMPTY;
  auto const& rhs = extractFunctionParameterValue(args, 1);
  auto const rhsValue = rhs.isString() ? iresearch::getBytesRef(rhs.slice())
                                       : irs::bytes_ref::EMPTY;

  size_t const dist = irs::edit_distance(description, lhsValue, rhsValue);

  return AqlValue(AqlValueHintBool(dist <= unsignedMaxDistanceValue));
}

/// @brief function IN_RANGE
AqlValue Functions::InRange(ExpressionContext* ctx, transaction::Methods* trx,
                            VPackFunctionParameters const& args) {
  static char const* AFN = "IN_RANGE";

  auto const argc = args.size();

  if (argc != 5) {
    registerWarning(ctx, AFN,
                    arangodb::Result{TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH,
                                     "5 arguments are expected."});
    return AqlValue(AqlValueHintNull());
  }

  auto const& attributeVal = extractFunctionParameterValue(args, 0);
  auto const& lowerVal = extractFunctionParameterValue(args, 1);
  auto const& upperVal = extractFunctionParameterValue(args, 2);
  auto const& includeLowerVal = extractFunctionParameterValue(args, 3);
  auto const& includeUpperVal = extractFunctionParameterValue(args, 4);

  if (ADB_UNLIKELY(!includeLowerVal.isBoolean())) {
    arangodb::aql::registerInvalidArgumentWarning(ctx, AFN);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }

  if (ADB_UNLIKELY(!includeUpperVal.isBoolean())) {
    arangodb::aql::registerInvalidArgumentWarning(ctx, AFN);
    return arangodb::aql::AqlValue{arangodb::aql::AqlValueHintNull{}};
  }

  auto const includeLower = includeLowerVal.toBoolean();
  auto const includeUpper = includeUpperVal.toBoolean();

  // first check lower bound
  {
    auto const compareLowerResult = AqlValue::Compare(trx, lowerVal, attributeVal, true);
    if ((!includeLower && compareLowerResult >= 0) ||
        (includeLower && compareLowerResult > 0)) {
      return AqlValue(AqlValueHintBool(false));
    }
  }

  // lower bound is fine, check upper
  auto const compareUpperResult = AqlValue::Compare(trx, attributeVal, upperVal, true);
  return AqlValue(AqlValueHintBool((includeUpper && compareUpperResult <= 0) ||
                                   (!includeUpper && compareUpperResult < 0)));
}

/// @brief function TO_BOOL
AqlValue Functions::ToBool(ExpressionContext*, transaction::Methods* /*trx*/,
                           VPackFunctionParameters const& parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.toBoolean()));
}

/// @brief function TO_ARRAY
AqlValue Functions::ToArray(ExpressionContext*, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (value.isArray()) {
    // return copy of the original array
    return value.clone();
  }

  if (value.isNull(true)) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  if (value.isBoolean() || value.isNumber() || value.isString()) {
    // return array with single member
    builder->add(value.slice());
  } else if (value.isObject()) {
    AqlValueMaterializer materializer(trx);
    VPackSlice slice = materializer.slice(value, false);
    // return an array with the attribute values
    for (auto it : VPackObjectIterator(slice, true)) {
      if (it.value.isCustom()) {
        builder->add(VPackValue(trx->extractIdString(slice)));
      } else {
        builder->add(it.value);
      }
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function LENGTH
AqlValue Functions::Length(ExpressionContext*, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (value.isArray()) {
    // shortcut!
    return AqlValue(AqlValueHintUInt(value.length()));
  }

  size_t length = 0;
  if (value.isNull(true)) {
    length = 0;
  } else if (value.isBoolean()) {
    if (value.toBoolean()) {
      length = 1;
    } else {
      length = 0;
    }
  } else if (value.isNumber()) {
    double tmp = value.toDouble();
    if (std::isnan(tmp) || !std::isfinite(tmp)) {
      length = 0;
    } else {
      char buffer[24];
      length = static_cast<size_t>(fpconv_dtoa(tmp, buffer));
    }
  } else if (value.isString()) {
    VPackValueLength l;
    char const* p = value.slice().getStringUnchecked(l);
    length = TRI_CharLengthUtf8String(p, l);
  } else if (value.isObject()) {
    length = static_cast<size_t>(value.length());
  }

  return AqlValue(AqlValueHintUInt(length));
}

/// @brief function FIND_FIRST
/// FIND_FIRST(text, search, start, end) → position
AqlValue Functions::FindFirst(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  static char const* AFN = "FIND_FIRST";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValue const& searchValue = extractFunctionParameterValue(parameters, 1);

  transaction::StringBufferLeaser buf1(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buf1->stringBuffer());
  ::appendAsString(trx, adapter, value);
  icu::UnicodeString uBuf(buf1->c_str(), static_cast<int32_t>(buf1->length()));

  transaction::StringBufferLeaser buf2(trx);
  arangodb::basics::VPackStringBufferAdapter adapter2(buf2->stringBuffer());
  ::appendAsString(trx, adapter2, searchValue);
  icu::UnicodeString uSearchBuf(buf2->c_str(), static_cast<int32_t>(buf2->length()));
  auto searchLen = uSearchBuf.length();

  int64_t startOffset = 0;
  int64_t maxEnd = -1;

  if (parameters.size() >= 3) {
    AqlValue const& optionalStartOffset = extractFunctionParameterValue(parameters, 2);
    startOffset = optionalStartOffset.toInt64();
    if (startOffset < 0) {
      return AqlValue(AqlValueHintInt(-1));
    }
  }

  maxEnd = uBuf.length();
  if (parameters.size() == 4) {
    AqlValue const& optionalEndMax = extractFunctionParameterValue(parameters, 3);
    if (!optionalEndMax.isNull(true)) {
      maxEnd = optionalEndMax.toInt64();
      if ((maxEnd < startOffset) || (maxEnd < 0)) {
        return AqlValue(AqlValueHintInt(-1));
      }
    }
  }

  if (searchLen == 0) {
    return AqlValue(AqlValueHintInt(startOffset));
  }
  if (uBuf.length() == 0) {
    return AqlValue(AqlValueHintInt(-1));
  }

  auto locale = LanguageFeature::instance()->getLocale();
  UErrorCode status = U_ZERO_ERROR;
  icu::StringSearch search(uSearchBuf, uBuf, locale, nullptr, status);

  for (int pos = search.first(status); U_SUCCESS(status) && pos != USEARCH_DONE;
       pos = search.next(status)) {
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    }
    if ((pos >= startOffset) && ((pos + searchLen - 1) <= maxEnd)) {
      return AqlValue(AqlValueHintInt(pos));
    }
  }
  return AqlValue(AqlValueHintInt(-1));
}

/// @brief function FIND_LAST
/// FIND_FIRST(text, search, start, end) → position
AqlValue Functions::FindLast(ExpressionContext* expressionContext, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  static char const* AFN = "FIND_LAST";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValue const& searchValue = extractFunctionParameterValue(parameters, 1);

  transaction::StringBufferLeaser buf1(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buf1->stringBuffer());
  ::appendAsString(trx, adapter, value);
  icu::UnicodeString uBuf(buf1->c_str(), static_cast<int32_t>(buf1->length()));

  transaction::StringBufferLeaser buf2(trx);
  arangodb::basics::VPackStringBufferAdapter adapter2(buf2->stringBuffer());
  ::appendAsString(trx, adapter2, searchValue);
  icu::UnicodeString uSearchBuf(buf2->c_str(), static_cast<int32_t>(buf2->length()));
  auto searchLen = uSearchBuf.length();

  int64_t startOffset = 0;
  int64_t maxEnd = -1;

  if (parameters.size() >= 3) {
    AqlValue const& optionalStartOffset = extractFunctionParameterValue(parameters, 2);
    startOffset = optionalStartOffset.toInt64();
    if (startOffset < 0) {
      return AqlValue(AqlValueHintInt(-1));
    }
  }

  maxEnd = uBuf.length();
  int emptySearchCludge = 0;
  if (parameters.size() == 4) {
    AqlValue const& optionalEndMax = extractFunctionParameterValue(parameters, 3);
    if (!optionalEndMax.isNull(true)) {
      maxEnd = optionalEndMax.toInt64();
      if ((maxEnd < startOffset) || (maxEnd < 0)) {
        return AqlValue(AqlValueHintInt(-1));
      }
      emptySearchCludge = 1;
    }
  }

  if (searchLen == 0) {
    return AqlValue(AqlValueHintInt(maxEnd + emptySearchCludge));
  }
  if (uBuf.length() == 0) {
    return AqlValue(AqlValueHintInt(-1));
  }

  auto locale = LanguageFeature::instance()->getLocale();
  UErrorCode status = U_ZERO_ERROR;
  icu::StringSearch search(uSearchBuf, uBuf, locale, nullptr, status);

  int foundPos = -1;
  for (int pos = search.first(status); U_SUCCESS(status) && pos != USEARCH_DONE;
       pos = search.next(status)) {
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    }
    if ((pos >= startOffset) && ((pos + searchLen - 1) <= maxEnd)) {
      foundPos = pos;
    }
  }
  return AqlValue(AqlValueHintInt(foundPos));
}

/// @brief function REVERSE
AqlValue Functions::Reverse(ExpressionContext* expressionContext, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  static char const* AFN = "REVERSE";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (value.isArray()) {
    transaction::BuilderLeaser builder(trx);
    AqlValueMaterializer materializer(trx);
    VPackSlice slice = materializer.slice(value, false);
    std::vector<VPackSlice> array;
    array.reserve(slice.length());
    for (VPackSlice it : VPackArrayIterator(slice)) {
      array.push_back(it);
    }
    std::reverse(std::begin(array), std::end(array));

    builder->openArray();
    for (auto const& it : array) {
      builder->add(it);
    }
    builder->close();
    return AqlValue(builder->slice(), builder->size());
  } else if (value.isString()) {
    std::string utf8;
    transaction::StringBufferLeaser buf1(trx);
    arangodb::basics::VPackStringBufferAdapter adapter(buf1->stringBuffer());
    ::appendAsString(trx, adapter, value);
    icu::UnicodeString uBuf(buf1->c_str(), static_cast<int32_t>(buf1->length()));
    // reserve the result buffer, but need to set empty afterwards:
    icu::UnicodeString result;
    result.getBuffer(uBuf.length());
    result = "";
    icu::StringCharacterIterator iter(uBuf, uBuf.length());
    UChar c = iter.previous();
    while (c != icu::CharacterIterator::DONE) {
      result.append(c);
      c = iter.previous();
    }
    result.toUTF8String(utf8);

    return AqlValue(utf8);
  } else {
    // neither array nor string...
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }
}

/// @brief function FIRST
AqlValue Functions::First(ExpressionContext* expressionContext, transaction::Methods*,
                          VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "FIRST";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  if (value.length() == 0) {
    return AqlValue(AqlValueHintNull());
  }

  bool mustDestroy;
  return value.at(0, mustDestroy, true);
}

/// @brief function LAST
AqlValue Functions::Last(ExpressionContext* expressionContext, transaction::Methods*,
                         VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "LAST";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  VPackValueLength const n = value.length();

  if (n == 0) {
    return AqlValue(AqlValueHintNull());
  }

  bool mustDestroy;
  return value.at(n - 1, mustDestroy, true);
}

/// @brief function NTH
AqlValue Functions::Nth(ExpressionContext* expressionContext, transaction::Methods*,
                        VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "NTH";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  VPackValueLength const n = value.length();

  if (n == 0) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& position = extractFunctionParameterValue(parameters, 1);
  int64_t index = position.toInt64();

  if (index < 0 || index >= static_cast<int64_t>(n)) {
    return AqlValue(AqlValueHintNull());
  }

  bool mustDestroy;
  return value.at(index, mustDestroy, true);
}

/// @brief function CONTAINS
AqlValue Functions::Contains(ExpressionContext*, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValue const& search = extractFunctionParameterValue(parameters, 1);
  AqlValue const& returnIndex = extractFunctionParameterValue(parameters, 2);

  bool const willReturnIndex = returnIndex.toBoolean();

  int result = -1;  // default is "not found"
  {
    transaction::StringBufferLeaser buffer(trx);
    arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

    ::appendAsString(trx, adapter, value);
    size_t const valueLength = buffer->length();

    size_t const searchOffset = buffer->length();
    ::appendAsString(trx, adapter, search);
    size_t const searchLength = buffer->length() - valueLength;

    if (searchLength > 0) {
      char const* found = static_cast<char const*>(
          memmem(buffer->c_str(), valueLength, buffer->c_str() + searchOffset, searchLength));

      if (found != nullptr) {
        if (willReturnIndex) {
          // find offset into string
          int bytePosition = static_cast<int>(found - buffer->c_str());
          char const* p = buffer->c_str();
          int pos = 0;
          while (pos < bytePosition) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c < 128) {
              ++pos;
            } else if (c < 224) {
              pos += 2;
            } else if (c < 240) {
              pos += 3;
            } else if (c < 248) {
              pos += 4;
            }
          }
          result = pos;
        } else {
          // fake result position, but it does not matter as it will
          // only be compared to -1 later
          result = 0;
        }
      }
    }
  }

  if (willReturnIndex) {
    // return numeric value
    return AqlValue(AqlValueHintInt(result));
  }

  // return boolean
  return AqlValue(AqlValueHintBool(result != -1));
}

/// @brief function CONCAT
AqlValue Functions::Concat(ExpressionContext*, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  size_t const n = parameters.size();

  if (n == 1) {
    AqlValue const& member = extractFunctionParameterValue(parameters, 0);
    if (member.isArray()) {
      AqlValueMaterializer materializer(trx);
      VPackSlice slice = materializer.slice(member, false);

      for (VPackSlice it : VPackArrayIterator(slice)) {
        if (it.isNull()) {
          continue;
        }
        // convert member to a string and append
        ::appendAsString(trx, adapter, AqlValue(it.begin()));
      }
      return AqlValue(buffer->c_str(), buffer->length());
    }
  }

  for (size_t i = 0; i < n; ++i) {
    AqlValue const& member = extractFunctionParameterValue(parameters, i);

    if (member.isNull(true)) {
      continue;
    }

    // convert member to a string and append
    ::appendAsString(trx, adapter, member);
  }

  return AqlValue(buffer->c_str(), buffer->length());
}

/// @brief function CONCAT_SEPARATOR
AqlValue Functions::ConcatSeparator(ExpressionContext*, transaction::Methods* trx,
                                    VPackFunctionParameters const& parameters) {
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  bool found = false;
  size_t const n = parameters.size();

  AqlValue const& separator = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(trx, adapter, separator);
  std::string const plainStr(buffer->c_str(), buffer->length());

  buffer->clear();

  if (n == 2) {
    AqlValue const& member = extractFunctionParameterValue(parameters, 1);

    if (member.isArray()) {
      // reserve *some* space
      buffer->reserve((plainStr.size() + 10) * member.length());

      AqlValueMaterializer materializer(trx);
      VPackSlice slice = materializer.slice(member, false);

      for (VPackSlice it : VPackArrayIterator(slice)) {
        if (it.isNull()) {
          continue;
        }
        if (found) {
          buffer->appendText(plainStr);
        }
        // convert member to a string and append
        ::appendAsString(trx, adapter, AqlValue(it.begin()));
        found = true;
      }
      return AqlValue(buffer->c_str(), buffer->length());
    }
  }

  // reserve *some* space
  buffer->reserve((plainStr.size() + 10) * n);
  for (size_t i = 1; i < n; ++i) {
    AqlValue const& member = extractFunctionParameterValue(parameters, i);

    if (member.isNull(true)) {
      continue;
    }
    if (found) {
      buffer->appendText(plainStr);
    }

    // convert member to a string and append
    ::appendAsString(trx, adapter, member);
    found = true;
  }

  return AqlValue(buffer->c_str(), buffer->length());
}

/// @brief function CHAR_LENGTH
AqlValue Functions::CharLength(ExpressionContext*, transaction::Methods* trx,
                               VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  size_t length = 0;

  if (value.isArray() || value.isObject()) {
    AqlValueMaterializer materializer(trx);
    VPackSlice slice = materializer.slice(value, false);

    transaction::StringBufferLeaser buffer(trx);
    arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

    VPackDumper dumper(&adapter, trx->transactionContextPtr()->getVPackOptions());
    dumper.dump(slice);

    length = buffer->length();

  } else if (value.isNull(true)) {
    length = 0;

  } else if (value.isBoolean()) {
    if (value.toBoolean()) {
      length = 4;
    } else {
      length = 5;
    }

  } else if (value.isNumber()) {
    double tmp = value.toDouble();
    if (std::isnan(tmp) || !std::isfinite(tmp)) {
      length = 0;
    } else {
      char buffer[24];
      length = static_cast<size_t>(fpconv_dtoa(tmp, buffer));
    }

  } else if (value.isString()) {
    VPackValueLength l;
    char const* p = value.slice().getStringUnchecked(l);
    length = TRI_CharLengthUtf8String(p, l);
  }

  return AqlValue(AqlValueHintUInt(length));
}

/// @brief function LOWER
AqlValue Functions::Lower(ExpressionContext*, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  std::string utf8;
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  icu::UnicodeString unicodeStr(buffer->c_str(), static_cast<int32_t>(buffer->length()));
  unicodeStr.toLower(nullptr);
  unicodeStr.toUTF8String(utf8);

  return AqlValue(utf8);
}

/// @brief function UPPER
AqlValue Functions::Upper(ExpressionContext*, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  std::string utf8;
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  icu::UnicodeString unicodeStr(buffer->c_str(), static_cast<int32_t>(buffer->length()));
  unicodeStr.toUpper(nullptr);
  unicodeStr.toUTF8String(utf8);

  return AqlValue(utf8);
}

/// @brief function SUBSTRING
AqlValue Functions::Substring(ExpressionContext*, transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  int32_t length = INT32_MAX;

  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);
  icu::UnicodeString unicodeStr(buffer->c_str(), static_cast<int32_t>(buffer->length()));

  int32_t offset =
      static_cast<int32_t>(extractFunctionParameterValue(parameters, 1).toInt64());

  if (parameters.size() == 3) {
    length = static_cast<int32_t>(extractFunctionParameterValue(parameters, 2).toInt64());
  }

  if (offset < 0) {
    offset = unicodeStr.moveIndex32(unicodeStr.moveIndex32(unicodeStr.length(), 0), offset);
  } else {
    offset = unicodeStr.moveIndex32(0, offset);
  }

  std::string utf8;
  unicodeStr
      .tempSubString(offset, unicodeStr.moveIndex32(offset, length) - offset)
      .toUTF8String(utf8);

  return AqlValue(utf8);
}
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

AqlValue Functions::Substitute(ExpressionContext* expressionContext,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& parameters) {
  static char const* AFN = "SUBSTITUTE";

  AqlValue const& search = extractFunctionParameterValue(parameters, 1);
  int64_t limit = -1;
  AqlValueMaterializer materializer(trx);
  std::vector<icu::UnicodeString> matchPatterns;
  std::vector<icu::UnicodeString> replacePatterns;
  bool replaceWasPlainString = false;

  if (search.isObject()) {
    if (parameters.size() > 3) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
    if (parameters.size() == 3) {
      limit = extractFunctionParameterValue(parameters, 2).toInt64();
    }
    VPackSlice slice = materializer.slice(search, false);
    matchPatterns.reserve(slice.length());
    replacePatterns.reserve(slice.length());
    for (auto it : VPackObjectIterator(slice)) {
      arangodb::velocypack::ValueLength length;
      char const* str = it.key.getString(length);
      matchPatterns.push_back(icu::UnicodeString(str, static_cast<int32_t>(length)));
      if (it.value.isNull()) {
        // null replacement value => replace with an empty string
        replacePatterns.push_back(icu::UnicodeString("", int32_t(0)));
      } else if (it.value.isString()) {
        // string case
        str = it.value.getStringUnchecked(length);
        replacePatterns.push_back(icu::UnicodeString(str, static_cast<int32_t>(length)));
      } else {
        // non strings
        registerInvalidArgumentWarning(expressionContext, AFN);
        return AqlValue(AqlValueHintNull());
      }
    }
  } else {
    if (parameters.size() < 2) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
    if (parameters.size() == 4) {
      limit = extractFunctionParameterValue(parameters, 3).toInt64();
    }

    VPackSlice slice = materializer.slice(search, false);
    if (search.isArray()) {
      for (VPackSlice it : VPackArrayIterator(slice)) {
        if (it.isString()) {
          arangodb::velocypack::ValueLength length;
          char const* str = it.getStringUnchecked(length);
          matchPatterns.push_back(icu::UnicodeString(str, static_cast<int32_t>(length)));
        } else {
          registerInvalidArgumentWarning(expressionContext, AFN);
          return AqlValue(AqlValueHintNull());
        }
      }
    } else {
      if (!search.isString()) {
        registerInvalidArgumentWarning(expressionContext, AFN);
        return AqlValue(AqlValueHintNull());
      }
      arangodb::velocypack::ValueLength length;

      char const* str = slice.getStringUnchecked(length);
      matchPatterns.push_back(icu::UnicodeString(str, static_cast<int32_t>(length)));
    }
    if (parameters.size() > 2) {
      AqlValue const& replace = extractFunctionParameterValue(parameters, 2);
      AqlValueMaterializer materializer2(trx);
      VPackSlice rslice = materializer2.slice(replace, false);
      if (replace.isArray()) {
        for (VPackSlice it : VPackArrayIterator(rslice)) {
          if (it.isNull()) {
            // null replacement value => replace with an empty string
            replacePatterns.push_back(icu::UnicodeString("", int32_t(0)));
          } else if (it.isString()) {
            arangodb::velocypack::ValueLength length;
            char const* str = it.getStringUnchecked(length);
            replacePatterns.push_back(icu::UnicodeString(str, static_cast<int32_t>(length)));
          } else {
            registerInvalidArgumentWarning(expressionContext, AFN);
            return AqlValue(AqlValueHintNull());
          }
        }
      } else if (replace.isString()) {
        // If we have a string as replacement,
        // it counts in for all found values.
        replaceWasPlainString = true;
        arangodb::velocypack::ValueLength length;
        char const* str = rslice.getString(length);
        replacePatterns.push_back(icu::UnicodeString(str, static_cast<int32_t>(length)));
      } else {
        registerInvalidArgumentWarning(expressionContext, AFN);
        return AqlValue(AqlValueHintNull());
      }
    }
  }

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if ((limit == 0) || (matchPatterns.size() == 0)) {
    // if the limit is 0, or we don't have any match pattern, return the source
    // string.
    return AqlValue(value);
  }

  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);
  icu::UnicodeString unicodeStr(buffer->c_str(), static_cast<int32_t>(buffer->length()));

  auto locale = LanguageFeature::instance()->getLocale();
  // we can't copy the search instances, thus use pointers:
  std::vector<std::unique_ptr<icu::StringSearch>> searchVec;
  searchVec.reserve(matchPatterns.size());
  UErrorCode status = U_ZERO_ERROR;
  for (auto const& searchStr : matchPatterns) {
    // create a vector of string searches
    searchVec.push_back(std::make_unique<icu::StringSearch>(searchStr, unicodeStr,
                                                            locale, nullptr, status));
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    }
  }

  std::vector<std::pair<int32_t, int32_t>> srchResultPtrs;
  std::string utf8;
  srchResultPtrs.reserve(matchPatterns.size());
  for (auto& search : searchVec) {
    // We now find the first hit for each search string.
    auto pos = search->first(status);
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    }

    int32_t len = 0;
    if (pos != USEARCH_DONE) {
      len = search->getMatchedLength();
    }
    srchResultPtrs.push_back(std::make_pair(pos, len));
  }

  icu::UnicodeString result;
  int32_t lastStart = 0;
  int64_t count = 0;
  while (true) {
    int which = -1;
    int32_t pos = USEARCH_DONE;
    int32_t mLen = 0;
    int i = 0;
    for (auto resultPair : srchResultPtrs) {
      // We locate the nearest matching search result.
      int32_t thisPos;
      thisPos = resultPair.first;
      if ((pos == USEARCH_DONE) || (pos > thisPos)) {
        if (thisPos != USEARCH_DONE) {
          pos = thisPos;
          which = i;
          mLen = resultPair.second;
        }
      }
      i++;
    }
    if (which == -1) {
      break;
    }
    // from last match to this match, copy the original string.
    result.append(unicodeStr, lastStart, pos - lastStart);
    if (replacePatterns.size() != 0) {
      if (replacePatterns.size() > (size_t)which) {
        result.append(replacePatterns[which]);
      } else if (replaceWasPlainString) {
        result.append(replacePatterns[0]);
      }
    }

    // lastStart is the place up to we searched the source string
    lastStart = pos + mLen;

    // we try to search the next occurance of this string
    auto& search = searchVec[which];
    pos = search->next(status);
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    }
    if (pos != USEARCH_DONE) {
      mLen = search->getMatchedLength();
    } else {
      mLen = -1;
    }
    srchResultPtrs[which] = std::make_pair(pos, mLen);

    which = 0;
    for (auto searchPair : srchResultPtrs) {
      // now we invalidate all search results that overlap with
      // our last search result and see whether we can find the
      // overlapped pattern again.
      // However, that mustn't overlap with the current lastStart
      // position either.
      int32_t thisPos;
      thisPos = searchPair.first;
      if ((thisPos != USEARCH_DONE) && (thisPos < lastStart)) {
        auto& search = searchVec[which];
        pos = thisPos;
        while ((pos < lastStart) && (pos != USEARCH_DONE)) {
          pos = search->next(status);
          if (U_FAILURE(status)) {
            ::registerICUWarning(expressionContext, AFN, status);
            return AqlValue(AqlValueHintNull());
          }
          if (pos != USEARCH_DONE) {
            mLen = search->getMatchedLength();
          }
          srchResultPtrs[which] = std::make_pair(pos, mLen);
        }
      }
      which++;
    }

    count++;
    if ((limit != -1) && (count >= limit)) {
      // Do we have a limit count?
      break;
    }
    // check whether none of our search objects has any more results
    bool allFound = true;
    for (auto resultPair : srchResultPtrs) {
      if (resultPair.first != USEARCH_DONE) {
        allFound = false;
        break;
      }
    }
    if (allFound) {
      break;
    }
  }
  // Append from the last found:
  result.append(unicodeStr, lastStart, unicodeStr.length() - lastStart);

  result.toUTF8String(utf8);
  return AqlValue(utf8);
}

/// @brief function LEFT str, length
AqlValue Functions::Left(ExpressionContext*, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  AqlValue value = extractFunctionParameterValue(parameters, 0);
  uint32_t length =
      static_cast<int32_t>(extractFunctionParameterValue(parameters, 1).toInt64());

  std::string utf8;
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  icu::UnicodeString unicodeStr(buffer->c_str(), static_cast<int32_t>(buffer->length()));
  icu::UnicodeString left =
      unicodeStr.tempSubString(0, unicodeStr.moveIndex32(0, length));

  left.toUTF8String(utf8);
  return AqlValue(utf8);
}

/// @brief function RIGHT
AqlValue Functions::Right(ExpressionContext*, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  AqlValue value = extractFunctionParameterValue(parameters, 0);
  uint32_t length =
      static_cast<int32_t>(extractFunctionParameterValue(parameters, 1).toInt64());

  std::string utf8;
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  icu::UnicodeString unicodeStr(buffer->c_str(), static_cast<int32_t>(buffer->length()));
  icu::UnicodeString right = unicodeStr.tempSubString(
      unicodeStr.moveIndex32(unicodeStr.length(), -static_cast<int32_t>(length)));

  right.toUTF8String(utf8);
  return AqlValue(utf8);
}

namespace {
void ltrimInternal(int32_t& startOffset, int32_t& endOffset, icu::UnicodeString& unicodeStr,
                   uint32_t numWhitespaces, UChar32* spaceChars) {
  for (; startOffset < endOffset; startOffset = unicodeStr.moveIndex32(startOffset, 1)) {
    bool found = false;

    for (uint32_t pos = 0; pos < numWhitespaces; pos++) {
      if (unicodeStr.char32At(startOffset) == spaceChars[pos]) {
        found = true;
        break;
      }
    }

    if (!found) {
      break;
    }
  }  // for
}

void rtrimInternal(int32_t& startOffset, int32_t& endOffset, icu::UnicodeString& unicodeStr,
                   uint32_t numWhitespaces, UChar32* spaceChars) {
  if (unicodeStr.length() == 0) {
    return;
  }
  for (int32_t codePos = unicodeStr.moveIndex32(endOffset, -1);
       startOffset <= codePos; codePos = unicodeStr.moveIndex32(codePos, -1)) {
    bool found = false;

    for (uint32_t pos = 0; pos < numWhitespaces; pos++) {
      if (unicodeStr.char32At(codePos) == spaceChars[pos]) {
        found = true;
        --endOffset;
        break;
      }
    }

    if (!found || codePos == 0) {
      break;
    }
  }  // for
}
}  // namespace

/// @brief function TRIM
AqlValue Functions::Trim(ExpressionContext* expressionContext, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "TRIM";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());
  ::appendAsString(trx, adapter, value);
  icu::UnicodeString unicodeStr(buffer->c_str(), static_cast<int32_t>(buffer->length()));

  int64_t howToTrim = 0;
  icu::UnicodeString whitespace("\r\n\t ");

  if (parameters.size() == 2) {
    AqlValue const& optional = extractFunctionParameterValue(parameters, 1);

    if (optional.isNumber()) {
      howToTrim = optional.toInt64();

      if (howToTrim < 0 || 2 < howToTrim) {
        howToTrim = 0;
      }
    } else if (optional.isString()) {
      buffer->clear();
      ::appendAsString(trx, adapter, optional);
      whitespace = icu::UnicodeString(buffer->c_str(),
                                      static_cast<int32_t>(buffer->length()));
    }
  }

  uint32_t numWhitespaces = whitespace.countChar32();
  UErrorCode errorCode = U_ZERO_ERROR;
  auto spaceChars = std::make_unique<UChar32[]>(numWhitespaces);

  whitespace.toUTF32(spaceChars.get(), numWhitespaces, errorCode);
  if (U_FAILURE(errorCode)) {
    ::registerICUWarning(expressionContext, AFN, errorCode);
    return AqlValue(AqlValueHintNull());
  }

  int32_t startOffset = 0, endOffset = unicodeStr.length();

  if (howToTrim <= 1) {
    ltrimInternal(startOffset, endOffset, unicodeStr, numWhitespaces, spaceChars.get());
  }

  if (howToTrim == 2 || howToTrim == 0) {
    rtrimInternal(startOffset, endOffset, unicodeStr, numWhitespaces, spaceChars.get());
  }

  icu::UnicodeString result = unicodeStr.tempSubString(startOffset, endOffset - startOffset);
  std::string utf8;
  result.toUTF8String(utf8);
  return AqlValue(utf8);
}

/// @brief function LTRIM
AqlValue Functions::LTrim(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "LTRIM";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());
  ::appendAsString(trx, adapter, value);
  icu::UnicodeString unicodeStr(buffer->c_str(), static_cast<int32_t>(buffer->length()));
  icu::UnicodeString whitespace("\r\n\t ");

  if (parameters.size() == 2) {
    AqlValue const& pWhitespace = extractFunctionParameterValue(parameters, 1);
    buffer->clear();
    ::appendAsString(trx, adapter, pWhitespace);
    whitespace =
        icu::UnicodeString(buffer->c_str(), static_cast<int32_t>(buffer->length()));
  }

  uint32_t numWhitespaces = whitespace.countChar32();
  UErrorCode errorCode = U_ZERO_ERROR;
  auto spaceChars = std::make_unique<UChar32[]>(numWhitespaces);

  whitespace.toUTF32(spaceChars.get(), numWhitespaces, errorCode);
  if (U_FAILURE(errorCode)) {
    ::registerICUWarning(expressionContext, AFN, errorCode);
    return AqlValue(AqlValueHintNull());
  }

  int32_t startOffset = 0, endOffset = unicodeStr.length();

  ltrimInternal(startOffset, endOffset, unicodeStr, numWhitespaces, spaceChars.get());

  icu::UnicodeString result = unicodeStr.tempSubString(startOffset, endOffset - startOffset);
  std::string utf8;
  result.toUTF8String(utf8);
  return AqlValue(utf8);
}

/// @brief function RTRIM
AqlValue Functions::RTrim(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "RTRIM";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());
  ::appendAsString(trx, adapter, value);
  icu::UnicodeString unicodeStr(buffer->c_str(), static_cast<int32_t>(buffer->length()));
  icu::UnicodeString whitespace("\r\n\t ");

  if (parameters.size() == 2) {
    AqlValue const& pWhitespace = extractFunctionParameterValue(parameters, 1);
    buffer->clear();
    ::appendAsString(trx, adapter, pWhitespace);
    whitespace =
        icu::UnicodeString(buffer->c_str(), static_cast<int32_t>(buffer->length()));
  }

  uint32_t numWhitespaces = whitespace.countChar32();
  UErrorCode errorCode = U_ZERO_ERROR;
  auto spaceChars = std::make_unique<UChar32[]>(numWhitespaces);

  whitespace.toUTF32(spaceChars.get(), numWhitespaces, errorCode);
  if (U_FAILURE(errorCode)) {
    ::registerICUWarning(expressionContext, AFN, errorCode);
    return AqlValue(AqlValueHintNull());
  }

  int32_t startOffset = 0, endOffset = unicodeStr.length();

  rtrimInternal(startOffset, endOffset, unicodeStr, numWhitespaces, spaceChars.get());

  icu::UnicodeString result = unicodeStr.tempSubString(startOffset, endOffset - startOffset);
  std::string utf8;
  result.toUTF8String(utf8);
  return AqlValue(utf8);
}

/// @brief function LIKE
AqlValue Functions::Like(ExpressionContext* expressionContext, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  static char const* AFN = "LIKE";

  bool const caseInsensitive = ::getBooleanParameter(trx, parameters, 2, false);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  // build pattern from parameter #1
  AqlValue const& regex = extractFunctionParameterValue(parameters, 1);
  ::appendAsString(trx, adapter, regex);

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher =
      expressionContext->buildLikeMatcher(buffer->c_str(), buffer->length(), caseInsensitive);

  if (matcher == nullptr) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  // extract value
  buffer->clear();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(trx, adapter, value);

  bool error = false;
  bool const result = arangodb::basics::Utf8Helper::DefaultUtf8Helper.matches(
      matcher, buffer->c_str(), buffer->length(), false, error);

  if (error) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(AqlValueHintBool(result));
}

/// @brief function SPLIT
AqlValue Functions::Split(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  static char const* AFN = "SPLIT";

  // cheapest parameter checks first:
  int64_t limitNumber = -1;
  if (parameters.size() == 3) {
    AqlValue const& aqlLimit = extractFunctionParameterValue(parameters, 2);
    if (aqlLimit.isNumber()) {
      limitNumber = aqlLimit.toInt64();
    } else {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    // these are edge cases which are documented to have these return values:
    if (limitNumber < 0) {
      return AqlValue(AqlValueHintNull());
    }
    if (limitNumber == 0) {
      return AqlValue(AqlValueHintEmptyArray());
    }
  }

  transaction::StringBufferLeaser regexBuffer(trx);
  AqlValue aqlSeparatorExpression;
  if (parameters.size() >= 2) {
    aqlSeparatorExpression = extractFunctionParameterValue(parameters, 1);
    if (aqlSeparatorExpression.isObject()) {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }
  }

  AqlValue const& aqlValueToSplit = extractFunctionParameterValue(parameters, 0);

  if (parameters.size() == 1) {
    // pre-documented edge-case: if we only have the first parameter, return it.
    transaction::BuilderLeaser builder(trx);
    builder->openArray();
    builder->add(aqlValueToSplit.slice());
    builder->close();
    return AqlValue(builder->slice(), builder->size());
  }

  // Get ready for ICU
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());
  Stringify(trx, adapter, aqlValueToSplit.slice());
  icu::UnicodeString valueToSplit(buffer->c_str(), static_cast<int32_t>(buffer->length()));
  bool isEmptyExpression = false;

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher =
      expressionContext->buildSplitMatcher(aqlSeparatorExpression,
                                           &trx->vpackOptions(), isEmptyExpression);

  if (matcher == nullptr) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser result(trx);
  result->openArray();
  if (!isEmptyExpression && (buffer->length() == 0)) {
    // Edge case: splitting an empty string by non-empty expression produces an
    // empty string again.
    result->add(VPackValue(""));
    result->close();
    return AqlValue(result->slice(), result->size());
  }

  std::string utf8;
  static const uint16_t nrResults = 16;
  icu::UnicodeString uResults[nrResults];
  int64_t totalCount = 0;
  while (true) {
    UErrorCode errorCode = U_ZERO_ERROR;
    auto uCount = matcher->split(valueToSplit, uResults, nrResults, errorCode);
    uint16_t copyThisTime = uCount;

    if (U_FAILURE(errorCode)) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
      return AqlValue(AqlValueHintNull());
    }

    if ((copyThisTime > 0) && (copyThisTime > nrResults)) {
      // last hit is the remaining string to be fed into split in a subsequent
      // invocation
      copyThisTime--;
    }

    if ((copyThisTime > 0) && ((copyThisTime == nrResults) || isEmptyExpression)) {
      // ICU will give us a traling empty string we don't care for if we split
      // with empty strings.
      copyThisTime--;
    }

    int64_t i = 0;
    while ((i < copyThisTime) && ((limitNumber < 0) || (totalCount < limitNumber))) {
      if ((i == 0) && isEmptyExpression) {
        // ICU will give us an empty string that we don't care for
        // as first value of one match-chunk
        i++;
        continue;
      }
      uResults[i].toUTF8String(utf8);
      result->add(VPackValue(utf8));
      utf8.clear();
      i++;
      totalCount++;
    }

    if (((uCount != nrResults)) ||  // fetch any / found less then N
        ((limitNumber >= 0) && (totalCount >= limitNumber))) {  // fetch N
      break;
    }
    // ok, we have more to parse in the last result slot, reiterate with it:
    if (uCount == nrResults) {
      valueToSplit = uResults[nrResults - 1];
    } else {
      // should not go beyound the last match!
      TRI_ASSERT(false);
      break;
    }
  }

  result->close();
  return AqlValue(result->slice(), result->size());
}

/// @brief function REGEX_MATCHES
AqlValue Functions::RegexMatches(ExpressionContext* expressionContext,
                                 transaction::Methods* trx,
                                 VPackFunctionParameters const& parameters) {
  static char const* AFN = "REGEX_MATCHES";

  AqlValue const& aqlValueToMatch = extractFunctionParameterValue(parameters, 0);

  if (parameters.size() == 1) {
    transaction::BuilderLeaser builder(trx);
    builder->openArray();
    builder->add(aqlValueToMatch.slice());
    builder->close();
    return AqlValue(builder->slice(), builder->size());
  }

  bool const caseInsensitive = ::getBooleanParameter(trx, parameters, 2, false);

  // build pattern from parameter #1
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  AqlValue const& regex = extractFunctionParameterValue(parameters, 1);
  ::appendAsString(trx, adapter, regex);
  bool isEmptyExpression = (buffer->length() == 0);

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher =
      expressionContext->buildRegexMatcher(buffer->c_str(), buffer->length(), caseInsensitive);

  if (matcher == nullptr) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  buffer->clear();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(trx, adapter, value);
  icu::UnicodeString valueToMatch(buffer->c_str(),
                                  static_cast<uint32_t>(buffer->length()));

  transaction::BuilderLeaser result(trx);
  result->openArray();

  if (!isEmptyExpression && (buffer->length() == 0)) {
    // Edge case: splitting an empty string by non-empty expression produces an
    // empty string again.
    result->add(VPackValue(""));
    result->close();
    return AqlValue(result->slice(), result->size());
  }

  UErrorCode status = U_ZERO_ERROR;

  matcher->reset(valueToMatch);
  bool find = matcher->find();
  if (!find) {
    return AqlValue(AqlValueHintNull());
  }

  for (int i = 0; i <= matcher->groupCount(); i++) {
    icu::UnicodeString match = matcher->group(i, status);
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    } else {
      std::string s;
      match.toUTF8String(s);
      result->add(VPackValue(s));
    }
  }

  result->close();
  return AqlValue(result->slice(), result->size());
}

/// @brief function REGEX_SPLIT
AqlValue Functions::RegexSplit(ExpressionContext* expressionContext,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& parameters) {
  static char const* AFN = "REGEX_SPLIT";

  int64_t limitNumber = -1;
  if (parameters.size() == 4) {
    AqlValue const& aqlLimit = extractFunctionParameterValue(parameters, 3);
    if (aqlLimit.isNumber()) {
      limitNumber = aqlLimit.toInt64();
    } else {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    if (limitNumber < 0) {
      return AqlValue(AqlValueHintNull());
    }
    if (limitNumber == 0) {
      return AqlValue(AqlValueHintEmptyArray());
    }
  }

  AqlValue const& aqlValueToSplit = extractFunctionParameterValue(parameters, 0);

  if (parameters.size() == 1) {
    // pre-documented edge-case: if we only have the first parameter, return it.
    transaction::BuilderLeaser builder(trx);
    builder->openArray();
    builder->add(aqlValueToSplit.slice());
    builder->close();
    return AqlValue(builder->slice(), builder->size());
  }

  bool const caseInsensitive = ::getBooleanParameter(trx, parameters, 2, false);

  // build pattern from parameter #1
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  AqlValue const& regex = extractFunctionParameterValue(parameters, 1);
  ::appendAsString(trx, adapter, regex);
  bool isEmptyExpression = (buffer->length() == 0);

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher =
      expressionContext->buildRegexMatcher(buffer->c_str(), buffer->length(), caseInsensitive);

  if (matcher == nullptr) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  buffer->clear();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(trx, adapter, value);
  icu::UnicodeString valueToSplit(buffer->c_str(), static_cast<int32_t>(buffer->length()));

  transaction::BuilderLeaser result(trx);
  result->openArray();
  if (!isEmptyExpression && (buffer->length() == 0)) {
    // Edge case: splitting an empty string by non-empty expression produces an
    // empty string again.
    result->add(VPackValue(""));
    result->close();
    return AqlValue(result->slice(), result->size());
  }

  std::string utf8;
  static const uint16_t nrResults = 16;
  icu::UnicodeString uResults[nrResults];
  int64_t totalCount = 0;
  while (true) {
    UErrorCode errorCode = U_ZERO_ERROR;
    auto uCount = matcher->split(valueToSplit, uResults, nrResults, errorCode);
    uint16_t copyThisTime = uCount;

    if (U_FAILURE(errorCode)) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
      return AqlValue(AqlValueHintNull());
    }

    if ((copyThisTime > 0) && (copyThisTime > nrResults)) {
      // last hit is the remaining string to be fed into split in a subsequent
      // invocation
      copyThisTime--;
    }

    if ((copyThisTime > 0) && ((copyThisTime == nrResults) || isEmptyExpression)) {
      // ICU will give us a traling empty string we don't care for if we split
      // with empty strings.
      copyThisTime--;
    }

    int64_t i = 0;
    while (i < copyThisTime && (limitNumber < 0 || totalCount < limitNumber)) {
      if ((i == 0) && isEmptyExpression) {
        // ICU will give us an empty string that we don't care for
        // as first value of one match-chunk
        i++;
        continue;
      }
      uResults[i].toUTF8String(utf8);
      result->add(VPackValue(utf8));
      utf8.clear();
      i++;
      totalCount++;
    }

    if (uCount != nrResults ||  // fetch any / found less then N
        (limitNumber >= 0 && totalCount >= limitNumber)) {  // fetch N
      break;
    }
    // ok, we have more to parse in the last result slot, reiterate with it:
    if (uCount == nrResults) {
      valueToSplit = uResults[nrResults - 1];
    } else {
      // should not go beyound the last match!
      TRI_ASSERT(false);
      break;
    }
  }

  result->close();
  return AqlValue(result->slice(), result->size());
}

/// @brief function REGEX_TEST
AqlValue Functions::RegexTest(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  static char const* AFN = "REGEX_TEST";

  bool const caseInsensitive = ::getBooleanParameter(trx, parameters, 2, false);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  // build pattern from parameter #1
  AqlValue const& regex = extractFunctionParameterValue(parameters, 1);
  ::appendAsString(trx, adapter, regex);

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher =
      expressionContext->buildRegexMatcher(buffer->c_str(), buffer->length(), caseInsensitive);

  if (matcher == nullptr) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  // extract value
  buffer->clear();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(trx, adapter, value);

  bool error = false;
  bool const result = arangodb::basics::Utf8Helper::DefaultUtf8Helper.matches(
      matcher, buffer->c_str(), buffer->length(), true, error);

  if (error) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(AqlValueHintBool(result));
}

/// @brief function REGEX_REPLACE
AqlValue Functions::RegexReplace(ExpressionContext* expressionContext,
                                 transaction::Methods* trx,
                                 VPackFunctionParameters const& parameters) {
  static char const* AFN = "REGEX_REPLACE";

  bool const caseInsensitive = ::getBooleanParameter(trx, parameters, 3, false);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  // build pattern from parameter #1
  AqlValue const& regex = extractFunctionParameterValue(parameters, 1);
  ::appendAsString(trx, adapter, regex);

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher =
      expressionContext->buildRegexMatcher(buffer->c_str(), buffer->length(), caseInsensitive);

  if (matcher == nullptr) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  // extract value
  buffer->clear();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(trx, adapter, value);

  size_t const split = buffer->length();
  AqlValue const& replace = extractFunctionParameterValue(parameters, 2);
  ::appendAsString(trx, adapter, replace);

  bool error = false;
  std::string result = arangodb::basics::Utf8Helper::DefaultUtf8Helper.replace(
      matcher, buffer->c_str(), split, buffer->c_str() + split,
      buffer->length() - split, false, error);

  if (error) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(result);
}

/// @brief function DATE_NOW
AqlValue Functions::DateNow(ExpressionContext*, transaction::Methods*,
                            VPackFunctionParameters const&) {
  auto millis = std::chrono::duration_cast<duration<int64_t, std::milli>>(
      system_clock::now().time_since_epoch());
  uint64_t dur = millis.count();
  return AqlValue(AqlValueHintUInt(dur));
}

/// @brief function DATE_ISO8601
AqlValue Functions::DateIso8601(ExpressionContext* expressionContext, transaction::Methods*,
                                VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_ISO8601";
  return ::dateFromParameters(expressionContext, parameters, AFN, false);
}

/// @brief function DATE_TIMESTAMP
AqlValue Functions::DateTimestamp(ExpressionContext* expressionContext,
                                  transaction::Methods*,
                                  VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_TIMESTAMP";
  return ::dateFromParameters(expressionContext, parameters, AFN, true);
}

/// @brief function IS_DATESTRING
AqlValue Functions::IsDatestring(ExpressionContext*, transaction::Methods*,
                                 VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  bool isValid = false;

  if (value.isString()) {
    tp_sys_clock_ms tp;  // unused
    isValid = basics::parseDateTime(value.slice().stringRef(), tp);
  }

  return AqlValue(AqlValueHintBool(isValid));
}

/// @brief function DATE_DAYOFWEEK
AqlValue Functions::DateDayOfWeek(ExpressionContext* expressionContext,
                                  transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_DAYOFWEEK";
  tp_sys_clock_ms tp;
  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }
  weekday wd{floor<days>(tp)};

  return AqlValue(AqlValueHintUInt(wd.c_encoding()));
}

/// @brief function DATE_YEAR
AqlValue Functions::DateYear(ExpressionContext* expressionContext, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_YEAR";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }
  auto ymd = year_month_day(floor<days>(tp));
  // Not the library has operator (int) implemented...
  int64_t year = static_cast<int64_t>((int)ymd.year());
  return AqlValue(AqlValueHintInt(year));
}

/// @brief function DATE_MONTH
AqlValue Functions::DateMonth(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_MONTH";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }
  auto ymd = year_month_day(floor<days>(tp));
  // The library has operator (unsigned) implemented
  uint64_t month = static_cast<uint64_t>((unsigned)ymd.month());
  return AqlValue(AqlValueHintUInt(month));
}

/// @brief function DATE_DAY
AqlValue Functions::DateDay(ExpressionContext* expressionContext, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_DAY";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto ymd = year_month_day(floor<days>(tp));
  // The library has operator (unsigned) implemented
  uint64_t day = static_cast<uint64_t>((unsigned)ymd.day());
  return AqlValue(AqlValueHintUInt(day));
}

/// @brief function DATE_HOUR
AqlValue Functions::DateHour(ExpressionContext* expressionContext, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_HOUR";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto day_time = make_time(tp - floor<days>(tp));
  uint64_t hours = day_time.hours().count();
  return AqlValue(AqlValueHintUInt(hours));
}

/// @brief function DATE_MINUTE
AqlValue Functions::DateMinute(ExpressionContext* expressionContext,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_MINUTE";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto day_time = make_time(tp - floor<days>(tp));
  uint64_t minutes = day_time.minutes().count();
  return AqlValue(AqlValueHintUInt(minutes));
}

/// @brief function DATE_SECOND
AqlValue Functions::DateSecond(ExpressionContext* expressionContext,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_SECOND";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto day_time = make_time(tp - floor<days>(tp));
  uint64_t seconds = day_time.seconds().count();
  return AqlValue(AqlValueHintUInt(seconds));
}

/// @brief function DATE_MILLISECOND
AqlValue Functions::DateMillisecond(ExpressionContext* expressionContext,
                                    transaction::Methods* trx,
                                    VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_MILLISECOND";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }
  auto day_time = make_time(tp - floor<days>(tp));
  uint64_t millis = day_time.subseconds().count();
  return AqlValue(AqlValueHintUInt(millis));
}

/// @brief function DATE_DAYOFYEAR
AqlValue Functions::DateDayOfYear(ExpressionContext* expressionContext,
                                  transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_DAYOFYEAR";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto ymd = year_month_day(floor<days>(tp));
  auto yyyy = year{ymd.year()};
  // we construct the date with the first day in the year:
  auto firstDayInYear = yyyy / jan / day{0};
  uint64_t daysSinceFirst = duration_cast<days>(tp - sys_days(firstDayInYear)).count();

  return AqlValue(AqlValueHintUInt(daysSinceFirst));
}

/// @brief function DATE_ISOWEEK
AqlValue Functions::DateIsoWeek(ExpressionContext* expressionContext,
                                transaction::Methods* trx,
                                VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_ISOWEEK";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  iso_week::year_weeknum_weekday yww{floor<days>(tp)};
  // The (unsigned) operator is overloaded...
  uint64_t isoWeek = static_cast<uint64_t>((unsigned)(yww.weeknum()));
  return AqlValue(AqlValueHintUInt(isoWeek));
}

/// @brief function DATE_LEAPYEAR
AqlValue Functions::DateLeapYear(ExpressionContext* expressionContext,
                                 transaction::Methods* trx,
                                 VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_LEAPYEAR";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  year_month_day ymd{floor<days>(tp)};

  return AqlValue(AqlValueHintBool(ymd.year().is_leap()));
}

/// @brief function DATE_QUARTER
AqlValue Functions::DateQuarter(ExpressionContext* expressionContext,
                                transaction::Methods* trx,
                                VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_QUARTER";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  year_month_day ymd{floor<days>(tp)};
  month m = ymd.month();

  // Library has unsigned operator implemented.
  uint64_t part = static_cast<uint64_t>(ceil(unsigned(m) / 3.0f));
  // We only have 4 quarters ;)
  TRI_ASSERT(part <= 4);
  return AqlValue(AqlValueHintUInt(part));
}

/// @brief function DATE_DAYS_IN_MONTH
AqlValue Functions::DateDaysInMonth(ExpressionContext* expressionContext,
                                    transaction::Methods* trx,
                                    VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_DAYS_IN_MONTH";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto ymd = year_month_day{floor<days>(tp)};
  auto lastMonthDay = ymd.year() / ymd.month() / last;

  // The Library has operator unsigned implemented
  return AqlValue(AqlValueHintUInt(static_cast<uint64_t>(unsigned(lastMonthDay.day()))));
}

/// @brief function DATE_TRUNC
AqlValue Functions::DateTrunc(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_TRUNC";
  using namespace std::chrono;
  using namespace date;

  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& durationType = extractFunctionParameterValue(parameters, 1);

  if (!durationType.isString()) {  // unit type must be string
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  std::string duration = durationType.slice().copyString();
  basics::StringUtils::tolowerInPlace(duration);

  year_month_day ymd{floor<days>(tp)};
  auto day_time = make_time(tp - sys_days(ymd));
  milliseconds ms{0};
  if (duration == "y" || duration == "year" || duration == "years") {
    ymd = year{ymd.year()} / jan / day{1};
  } else if (duration == "m" || duration == "month" || duration == "months") {
    ymd = year{ymd.year()} / ymd.month() / day{1};
  } else if (duration == "d" || duration == "day" || duration == "days") {
    // this would be: ymd = year{ymd.year()}/ymd.month()/ymd.day();
    // However, we already split ymd to the precision of days,
    // and ms to cary the timestamp part, so nothing needs to be done here.
  } else if (duration == "h" || duration == "hour" || duration == "hours") {
    ms = day_time.hours();
  } else if (duration == "i" || duration == "minute" || duration == "minutes") {
    ms = day_time.hours() + day_time.minutes();
  } else if (duration == "s" || duration == "second" || duration == "seconds") {
    ms = day_time.to_duration() - day_time.subseconds();
  } else if (duration == "f" || duration == "millisecond" ||
             duration == "milliseconds") {
    ms = day_time.to_duration();
  } else {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_DATE_VALUE);
    return AqlValue(AqlValueHintNull());
  }
  tp = tp_sys_clock_ms{sys_days(ymd) + ms};

  return ::timeAqlValue(expressionContext, AFN, tp);
}

/// @brief function DATE_UTCTOLOCAL
AqlValue Functions::DateUtcToLocal(ExpressionContext* expressionContext,
                                   transaction::Methods* trx,
                                   VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_UTCTOLOCAL";
  using namespace std::chrono;
  using namespace date;

  tp_sys_clock_ms tp_utc;

  if (!::parameterToTimePoint(expressionContext, parameters, tp_utc, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& timeZoneParam = extractFunctionParameterValue(parameters, 1);

  if (!timeZoneParam.isString()) {  // timezone type must be string
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  const std::string tz = timeZoneParam.slice().copyString();
  const auto utc = floor<milliseconds>(tp_utc);
  const auto zoned = make_zoned(tz, utc);
  const auto tp_local = tp_sys_clock_ms{zoned.get_local_time().time_since_epoch()};

  auto info = zoned.get_info();

  return ::timeAqlValue(expressionContext, AFN, tp_local,info.offset.count() == 0 && info.save.count() == 0);
}


/// @brief function DATE_LOCALTOUTC
AqlValue Functions::DateLocalToUtc(ExpressionContext* expressionContext,
                                   transaction::Methods* trx,
                                   VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_LOCALTOUTC";
  using namespace std::chrono;
  using namespace date;

  tp_sys_clock_ms tp_local;

  if (!::parameterToTimePoint(expressionContext, parameters, tp_local, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& timeZoneParam = extractFunctionParameterValue(parameters, 1);

  if (!timeZoneParam.isString()) {  // timezone type must be string
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  const std::string tz = timeZoneParam.slice().copyString();

  const auto local = local_time<milliseconds>{floor<milliseconds>(tp_local).time_since_epoch()};
  const auto zoned = make_zoned(tz, local);
  const auto tp_utc = tp_sys_clock_ms{zoned.get_sys_time().time_since_epoch()};

  return ::timeAqlValue(expressionContext, AFN, tp_utc);
}

/// @brief function DATE_ADD
AqlValue Functions::DateAdd(ExpressionContext* expressionContext, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_ADD";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  // size == 3 unit / unit type
  // size == 2 iso duration

  if (parameters.size() == 3) {
    AqlValue const& durationUnit = extractFunctionParameterValue(parameters, 1);
    if (!durationUnit.isNumber()) {  // unit must be number
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    AqlValue const& durationType = extractFunctionParameterValue(parameters, 2);
    if (!durationType.isString()) {  // unit type must be string
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    // Numbers and Strings can both be sliced
    return ::addOrSubtractUnitFromTimestamp(expressionContext, tp, durationUnit.slice(),
                                            durationType.slice(), AFN, false);
  } else {  // iso duration
    AqlValue const& isoDuration = extractFunctionParameterValue(parameters, 1);
    if (!isoDuration.isString()) {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    return ::addOrSubtractIsoDurationFromTimestamp(expressionContext, tp,
                                                   isoDuration.slice().stringRef(),
                                                   AFN, false);
  }
}

/// @brief function DATE_SUBTRACT
AqlValue Functions::DateSubtract(ExpressionContext* expressionContext,
                                 transaction::Methods* trx,
                                 VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_SUBTRACT";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  // size == 3 unit / unit type
  // size == 2 iso duration

  if (parameters.size() == 3) {
    AqlValue const& durationUnit = extractFunctionParameterValue(parameters, 1);
    if (!durationUnit.isNumber()) {  // unit must be number
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    AqlValue const& durationType = extractFunctionParameterValue(parameters, 2);
    if (!durationType.isString()) {  // unit type must be string
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    // Numbers and Strings can both be sliced
    return ::addOrSubtractUnitFromTimestamp(expressionContext, tp, durationUnit.slice(),
                                            durationType.slice(), AFN, true);
  } else {  // iso duration
    AqlValue const& isoDuration = extractFunctionParameterValue(parameters, 1);
    if (!isoDuration.isString()) {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    return ::addOrSubtractIsoDurationFromTimestamp(expressionContext, tp,
                                                   isoDuration.slice().stringRef(),
                                                   AFN, true);
  }
}

/// @brief function DATE_DIFF
AqlValue Functions::DateDiff(ExpressionContext* expressionContext, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_DIFF";
  // Extract first date
  tp_sys_clock_ms tp1;
  if (!::parameterToTimePoint(expressionContext, parameters, tp1, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  // Extract second date
  tp_sys_clock_ms tp2;
  if (!::parameterToTimePoint(expressionContext, parameters, tp2, AFN, 1)) {
    return AqlValue(AqlValueHintNull());
  }

  double diff = 0.0;
  bool asFloat = false;
  auto diffDuration = tp2 - tp1;

  AqlValue const& unitValue = extractFunctionParameterValue(parameters, 2);
  if (!unitValue.isString()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  DateSelectionModifier flag = ::parseDateModifierFlag(unitValue.slice());

  if (parameters.size() == 4) {
    AqlValue const& asFloatValue = extractFunctionParameterValue(parameters, 3);
    if (!asFloatValue.isBoolean()) {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }
    asFloat = asFloatValue.toBoolean();
  }

  switch (flag) {
    case YEAR:
      diff =
          duration_cast<duration<double, std::ratio_multiply<std::ratio<146097, 400>, days::period>>>(diffDuration)
              .count();
      break;
    case MONTH:
      diff = duration_cast<duration<double, std::ratio_divide<years::period, std::ratio<12>>>>(diffDuration)
                 .count();
      break;
    case WEEK:
      diff = duration_cast<duration<double, std::ratio_multiply<std::ratio<7>, days::period>>>(diffDuration)
                 .count();
      break;
    case DAY:
      diff = duration_cast<duration<double, std::ratio_multiply<std::ratio<24>, std::chrono::hours::period>>>(
                 diffDuration)
                 .count();
      break;
    case HOUR:
      diff = duration_cast<duration<double, std::ratio<3600>>>(diffDuration).count();
      break;
    case MINUTE:
      diff = duration_cast<duration<double, std::ratio<60>>>(diffDuration).count();
      break;
    case SECOND:
      diff = duration_cast<duration<double>>(diffDuration).count();
      break;
    case MILLI:
      diff = duration_cast<duration<double, std::milli>>(diffDuration).count();
      break;
    case INVALID:
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return AqlValue(AqlValueHintNull());
  }

  if (asFloat) {
    return AqlValue(AqlValueHintDouble(diff));
  }
  return AqlValue(AqlValueHintInt(static_cast<int64_t>(std::round(diff))));
}

/// @brief function DATE_COMPARE
AqlValue Functions::DateCompare(ExpressionContext* expressionContext,
                                transaction::Methods* trx,
                                VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_COMPARE";
  tp_sys_clock_ms tp1;
  if (!::parameterToTimePoint(expressionContext, parameters, tp1, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  tp_sys_clock_ms tp2;
  if (!::parameterToTimePoint(expressionContext, parameters, tp2, AFN, 1)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& rangeStartValue = extractFunctionParameterValue(parameters, 2);

  DateSelectionModifier rangeStart = ::parseDateModifierFlag(rangeStartValue.slice());

  if (rangeStart == INVALID) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  DateSelectionModifier rangeEnd = rangeStart;
  if (parameters.size() == 4) {
    AqlValue const& rangeEndValue = extractFunctionParameterValue(parameters, 3);
    rangeEnd = ::parseDateModifierFlag(rangeEndValue.slice());

    if (rangeEnd == INVALID) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
  }
  auto ymd1 = year_month_day{floor<days>(tp1)};
  auto ymd2 = year_month_day{floor<days>(tp2)};
  auto time1 = make_time(tp1 - floor<days>(tp1));
  auto time2 = make_time(tp2 - floor<days>(tp2));

  // This switch has the following feature:
  // It is ordered by the Highest value of
  // the Modifier (YEAR) and flows down to
  // lower values.
  // In each case if the value is significant
  // (above or equal the endRange) we compare it.
  // If this part is not equal we return false.
  // Otherwise we fall down to the next part.
  // As soon as we are below the endRange
  // we bail out.
  // So all Fall throughs here are intentional
  switch (rangeStart) {
    case YEAR:
      // Always check for the year
      if (ymd1.year() != ymd2.year()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case MONTH:
      if (rangeEnd > MONTH) {
        break;
      }
      if (ymd1.month() != ymd2.month()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case DAY:
      if (rangeEnd > DAY) {
        break;
      }
      if (ymd1.day() != ymd2.day()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case HOUR:
      if (rangeEnd > HOUR) {
        break;
      }
      if (time1.hours() != time2.hours()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case MINUTE:
      if (rangeEnd > MINUTE) {
        break;
      }
      if (time1.minutes() != time2.minutes()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case SECOND:
      if (rangeEnd > SECOND) {
        break;
      }
      if (time1.seconds() != time2.seconds()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case MILLI:
      if (rangeEnd > MILLI) {
        break;
      }
      if (time1.subseconds() != time2.subseconds()) {
        return AqlValue(AqlValueHintBool(false));
      }
      break;
    case INVALID:
    case WEEK:
      // Was handled before
      TRI_ASSERT(false);
  }

  // If we get here all significant places are equal
  // Name these two dates as equal
  return AqlValue(AqlValueHintBool(true));
}

/// @brief function DATE_ROUND
AqlValue Functions::DateRound(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  static char const* AFN = "DATE_ROUND";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& durationUnit = extractFunctionParameterValue(parameters, 1);
  if (!durationUnit.isNumber()) {  // unit must be number
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& durationType = extractFunctionParameterValue(parameters, 2);
  if (!durationType.isString()) {  // unit type must be string
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  int64_t const m = durationUnit.toInt64();
  if (m <= 0) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  velocypack::StringRef s = durationType.slice().stringRef();

  int64_t factor = 1;
  if (s == "milliseconds" || s == "millisecond" || s == "f") {
    factor = 1;
  } else if (s == "seconds" || s == "second" || s == "s") {
    factor = 1000;
  } else if (s == "minutes" || s == "minute" || s == "i") {
    factor = 60 * 1000;
  } else if (s == "hours" || s == "hour" || s == "h") {
    factor = 60 * 60 * 1000;
  } else if (s == "days" || s == "day" || s == "d") {
    factor = 24 * 60 * 60 * 1000;
  } else {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  int64_t const multiplier = factor * m;

  duration<int64_t, std::milli> time = tp.time_since_epoch();
  int64_t t = time.count();
  // integer division!
  t /= multiplier;
  tp = tp_sys_clock_ms(milliseconds(t * multiplier));
  return ::timeAqlValue(expressionContext, AFN, tp);
}

/// @brief function PASSTHRU
AqlValue Functions::Passthru(ExpressionContext*, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  if (parameters.empty()) {
    return AqlValue(AqlValueHintNull());
  }

  return extractFunctionParameterValue(parameters, 0).clone();
}

/// @brief function UNSET
AqlValue Functions::Unset(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  static char const* AFN = "UNSET";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isObject()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  std::unordered_set<std::string> names;
  ::extractKeys(names, expressionContext, trx, parameters, 1, AFN);

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);
  transaction::BuilderLeaser builder(trx);
  ::unsetOrKeep(trx, slice, names, true, false, *builder.get());
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function UNSET_RECURSIVE
AqlValue Functions::UnsetRecursive(ExpressionContext* expressionContext,
                                   transaction::Methods* trx,
                                   VPackFunctionParameters const& parameters) {
  static char const* AFN = "UNSET_RECURSIVE";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isObject()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  std::unordered_set<std::string> names;
  ::extractKeys(names, expressionContext, trx, parameters, 1, AFN);

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);
  transaction::BuilderLeaser builder(trx);
  ::unsetOrKeep(trx, slice, names, true, true, *builder.get());
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function KEEP
AqlValue Functions::Keep(ExpressionContext* expressionContext, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  static char const* AFN = "KEEP";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isObject()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  std::unordered_set<std::string> names;
  ::extractKeys(names, expressionContext, trx, parameters, 1, AFN);

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);
  transaction::BuilderLeaser builder(trx);
  ::unsetOrKeep(trx, slice, names, false, false, *builder.get());
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function TRANSLATE
AqlValue Functions::Translate(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "TRANSLATE";

  AqlValue const& key = extractFunctionParameterValue(parameters, 0);
  AqlValue const& lookupDocument = extractFunctionParameterValue(parameters, 1);

  if (!lookupDocument.isObject()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(lookupDocument, true);
  TRI_ASSERT(slice.isObject());

  VPackSlice result;
  if (key.isString()) {
    result = slice.get(key.slice().copyString());
  } else {
    transaction::StringBufferLeaser buffer(trx);
    arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());
    Functions::Stringify(trx, adapter, key.slice());
    result = slice.get(buffer->toString());
  }

  if (!result.isNone()) {
    return AqlValue(result);
  }

  // attribute not found, now return the default value
  // we must create copy of it however
  AqlValue const& defaultValue = extractFunctionParameterValue(parameters, 2);
  if (defaultValue.isNone()) {
    return key.clone();
  }
  return defaultValue.clone();
}

/// @brief function MERGE
AqlValue Functions::Merge(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  return ::mergeParameters(expressionContext, trx, parameters, "MERGE", false);
}

/// @brief function MERGE_RECURSIVE
AqlValue Functions::MergeRecursive(ExpressionContext* expressionContext,
                                   transaction::Methods* trx,
                                   VPackFunctionParameters const& parameters) {
  return ::mergeParameters(expressionContext, trx, parameters, "MERGE_RECURSIVE", true);
}

/// @brief function HAS
AqlValue Functions::Has(ExpressionContext*, transaction::Methods* trx,
                        VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();
  if (n < 2) {
    // no parameters
    return AqlValue(AqlValueHintBool(false));
  }

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isObject()) {
    // not an object
    return AqlValue(AqlValueHintBool(false));
  }

  AqlValue const& name = extractFunctionParameterValue(parameters, 1);
  std::string p;
  if (!name.isString()) {
    transaction::StringBufferLeaser buffer(trx);
    arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());
    ::appendAsString(trx, adapter, name);
    p = std::string(buffer->c_str(), buffer->length());
  } else {
    p = name.slice().copyString();
  }

  return AqlValue(AqlValueHintBool(value.hasKey(p)));
}

/// @brief function ATTRIBUTES
AqlValue Functions::Attributes(ExpressionContext* expressionContext,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (!value.isObject()) {
    // not an object
    registerWarning(expressionContext, "ATTRIBUTES",
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  bool const removeInternal = ::getBooleanParameter(trx, parameters, 1, false);
  bool const doSort = ::getBooleanParameter(trx, parameters, 2, false);

  TRI_ASSERT(value.isObject());
  if (value.length() == 0) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  if (doSort) {
    std::set<std::string, arangodb::basics::VelocyPackHelper::AttributeSorterUTF8> keys;

    VPackCollection::keys(slice, keys);
    transaction::BuilderLeaser builder(trx);
    builder->openArray();
    for (auto const& it : keys) {
      TRI_ASSERT(!it.empty());
      if (removeInternal && !it.empty() && it.at(0) == '_') {
        continue;
      }
      builder->add(VPackValue(it));
    }
    builder->close();

    return AqlValue(builder->slice(), builder->size());
  }

  std::unordered_set<std::string> keys;
  VPackCollection::keys(slice, keys);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : keys) {
    if (removeInternal && !it.empty() && it.at(0) == '_') {
      continue;
    }
    builder->add(VPackValue(it));
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function VALUES
AqlValue Functions::Values(ExpressionContext* expressionContext, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (!value.isObject()) {
    // not an object
    registerWarning(expressionContext, "VALUES", TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  bool const removeInternal = ::getBooleanParameter(trx, parameters, 1, false);

  TRI_ASSERT(value.isObject());
  if (value.length() == 0) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);
  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& entry : VPackObjectIterator(slice, true)) {
    if (!entry.key.isString()) {
      // somehow invalid
      continue;
    }
    if (removeInternal) {
      VPackValueLength l;
      char const* p = entry.key.getStringUnchecked(l);
      if (l > 0 && *p == '_') {
        // skip attribute
        continue;
      }
    }
    if (entry.value.isCustom()) {
      builder->add(VPackValue(trx->extractIdString(slice)));
    } else {
      builder->add(entry.value);
    }
  }
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function MIN
AqlValue Functions::Min(ExpressionContext* expressionContext, transaction::Methods* trx,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, "MIN", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  VPackSlice minValue;
  auto options = trx->transactionContextPtr()->getVPackOptions();
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (it.isNull()) {
      continue;
    }
    if (minValue.isNone() ||
        arangodb::basics::VelocyPackHelper::compare(it, minValue, true, options) < 0) {
      minValue = it;
    }
  }
  if (minValue.isNone()) {
    return AqlValue(AqlValueHintNull());
  }
  return AqlValue(minValue);
}

/// @brief function MAX
AqlValue Functions::Max(ExpressionContext* expressionContext, transaction::Methods* trx,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, "MAX", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);
  VPackSlice maxValue;
  auto options = trx->transactionContextPtr()->getVPackOptions();
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (maxValue.isNone() ||
        arangodb::basics::VelocyPackHelper::compare(it, maxValue, true, options) > 0) {
      maxValue = it;
    }
  }
  if (maxValue.isNone()) {
    return AqlValue(AqlValueHintNull());
  }
  return AqlValue(maxValue);
}

/// @brief function SUM
AqlValue Functions::Sum(ExpressionContext* expressionContext, transaction::Methods* trx,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, "SUM", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);
  double sum = 0.0;
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (it.isNull()) {
      continue;
    }
    if (!it.isNumber()) {
      return AqlValue(AqlValueHintNull());
    }
    double const number = it.getNumericValue<double>();

    if (!std::isnan(number) && number != HUGE_VAL && number != -HUGE_VAL) {
      sum += number;
    }
  }

  return ::numberValue(sum, false);
}

/// @brief function AVERAGE
AqlValue Functions::Average(ExpressionContext* expressionContext, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  static char const* AFN = "AVERAGE";
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  double sum = 0.0;
  size_t count = 0;
  for (VPackSlice v : VPackArrayIterator(slice)) {
    if (v.isNull()) {
      continue;
    }
    if (!v.isNumber()) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
      return AqlValue(AqlValueHintNull());
    }

    // got a numeric value
    double const number = v.getNumericValue<double>();

    if (!std::isnan(number) && number != HUGE_VAL && number != -HUGE_VAL) {
      sum += number;
      ++count;
    }
  }

  if (count > 0 && !std::isnan(sum) && sum != HUGE_VAL && sum != -HUGE_VAL) {
    return ::numberValue(sum / static_cast<size_t>(count), false);
  }

  return AqlValue(AqlValueHintNull());
}

/// @brief function PRODUCT
AqlValue Functions::Product(ExpressionContext* expressionContext, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, "PRODUCT", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);
  double product = 1.0;
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (it.isNull()) {
      continue;
    }
    if (!it.isNumber()) {
      return AqlValue(AqlValueHintNull());
    }
    double const number = it.getNumericValue<double>();

    if (!std::isnan(number) && number != HUGE_VAL && number != -HUGE_VAL) {
      product *= number;
    }
  }

  return ::numberValue(product, false);
}

/// @brief function SLEEP
AqlValue Functions::Sleep(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isNumber() || value.toDouble() < 0) {
    registerWarning(expressionContext, "SLEEP", TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  TRI_ASSERT(trx != nullptr);
  auto& server = trx->vocbase().server();

  double const sleepValue = value.toDouble();
  auto now = std::chrono::steady_clock::now();
  auto const endTime =
      now + std::chrono::milliseconds(static_cast<int64_t>(sleepValue * 1000.0));

  while (now < endTime) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (expressionContext->killed()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
    } else if (server.isStopping()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
    }
    now = std::chrono::steady_clock::now();
  }
  return AqlValue(AqlValueHintNull());
}

/// @brief function COLLECTIONS
AqlValue Functions::Collections(ExpressionContext* expressionContext,
                                transaction::Methods* trx,
                                VPackFunctionParameters const& parameters) {
  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  auto& vocbase = expressionContext->vocbase();
  auto colls = GetCollections(vocbase);

  std::sort(colls.begin(), colls.end(),
            [](std::shared_ptr<LogicalCollection> const& lhs,
               std::shared_ptr<LogicalCollection> const& rhs) -> bool {
              return arangodb::basics::StringUtils::tolower(lhs->name()) <
                     arangodb::basics::StringUtils::tolower(rhs->name());
            });

  size_t const n = colls.size();

  auto const& exec = ExecContext::current();
  for (size_t i = 0; i < n; ++i) {
    auto& coll = colls[i];

    if (!exec.canUseCollection(vocbase.name(), coll->name(), auth::Level::RO)) {
      continue;
    }

    builder->openObject();
    builder->add("_id", VPackValue(std::to_string(coll->id().id())));
    builder->add("name", VPackValue(coll->name()));
    builder->close();
  }

  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function RANDOM_TOKEN
AqlValue Functions::RandomToken(ExpressionContext*, transaction::Methods*,
                                VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  int64_t const length = value.toInt64();
  if (length < 0 || length > 65536) {
    THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                                  "RANDOM_TOKEN");
  }

  UniformCharacter generator(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
  return AqlValue(generator.random(static_cast<size_t>(length)));
}

/// @brief function IPV4_FROM_NUMBER
AqlValue Functions::IpV4FromNumber(ExpressionContext* expressionContext, transaction::Methods*,
                                   VPackFunctionParameters const& parameters) {
  static char const* AFN = "IPV4_FROM_NUMBER";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isNumber()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }
  
  int64_t input = value.toInt64();
  if (input < 0 || static_cast<uint64_t>(input) > UINT32_MAX) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  uint64_t number = static_cast<uint64_t>(input);

  // in theory, we only need a 15 bytes buffer here, as the maximum result
  // string is "255.255.255.255"
  char result[32]; 

  char* p = &result[0];
  // first part
  uint64_t digit = (number & 0xff000000ULL) >> 24ULL; 
  p += basics::StringUtils::itoa(digit, p);
  *p++ = '.';
  // second part
  digit = (number & 0x00ff0000ULL) >> 16ULL; 
  p += basics::StringUtils::itoa(digit, p);
  *p++ = '.';
  // third part
  digit = (number & 0x0000ff00ULL) >> 8ULL; 
  p += basics::StringUtils::itoa(digit, p);
  *p++ = '.';
  // fourth part
  digit = (number & 0x0000ffULL); 
  p += basics::StringUtils::itoa(digit, p);

  return AqlValue(&result[0], p - &result[0]);
}

/// @brief function IPV4_TO_NUMBER
AqlValue Functions::IpV4ToNumber(ExpressionContext* expressionContext, transaction::Methods* trx,
                                 VPackFunctionParameters const& parameters) {
  static char const* AFN = "IPV4_TO_NUMBER";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isString()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  // parse the input string
  TRI_ASSERT(slice.isString());
  VPackValueLength l;
  char const* p = slice.getString(l);

  if (l >= 7 && l <= 15) {
    // min value is 0.0.0.0 (length = 7)
    // max value is 255.255.255.255 (length = 15)
    char buffer[16];
    memcpy(&buffer[0], p, l);
    // null-terminate the buffer
    buffer[l] = '\0';

    struct in_addr addr;
    memset(&addr, 0, sizeof(struct in_addr));
#if _WIN32
    int result = InetPton(AF_INET, &buffer[0], &addr);
#else
    int result = inet_pton(AF_INET, &buffer[0], &addr);
#endif

#ifdef __APPLE__
    // inet_pton on MacOS accepts leading zeros...
    // inet_pton on Linux and Windows doesn't
    // this is the least intrusive solution, but it is not efficient.
    if (result == 1 && 
        std::regex_match(&buffer[0], buffer + l, ::ipV4LeadingZerosRegex, std::regex_constants::match_any)) {
      result = 0;
    }
#endif
    
    if (result == 1) {
      return AqlValue(AqlValueHintUInt(basics::hostToBig(*reinterpret_cast<uint32_t*>(&addr))));
    }
  }

  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

/// @brief function IS_IPV4
AqlValue Functions::IsIpV4(ExpressionContext* expressionContext, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isString()) {
    return AqlValue(AqlValueHintBool(false));
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);
  
  // parse the input string
  TRI_ASSERT(slice.isString());
  VPackValueLength l;
  char const* p = slice.getString(l);

  if (l >= 7 && l <= 15) {
    // min value is 0.0.0.0 (length = 7)
    // max value is 255.255.255.255 (length = 15)
    char buffer[16];
    memcpy(&buffer[0], p, l);
    // null-terminate the buffer
    buffer[l] = '\0';

    struct in_addr addr;
    memset(&addr, 0, sizeof(struct in_addr));
#if _WIN32
    int result = InetPton(AF_INET, &buffer[0], &addr);
#else
    int result = inet_pton(AF_INET, &buffer[0], &addr);
#endif
   
    if (result == 1) {
#ifdef __APPLE__
      // inet_pton on MacOS accepts leading zeros...
      // inet_pton on Linux and Windows doesn't
      // this is the least intrusive solution, but it is not efficient.
      if (std::regex_match(&buffer[0], buffer + l, ::ipV4LeadingZerosRegex, std::regex_constants::match_any)) {
        return AqlValue(AqlValueHintBool(false));
      }
#endif
      return AqlValue(AqlValueHintBool(true));
    }
  }
      
  return AqlValue(AqlValueHintBool(false));
}

/// @brief function MD5
AqlValue Functions::Md5(ExpressionContext*, transaction::Methods* trx,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  // create md5
  char hash[17];
  char* p = &hash[0];
  size_t length;

  arangodb::rest::SslInterface::sslMD5(buffer->c_str(), buffer->length(), p, length);

  // as hex
  char hex[33];
  p = &hex[0];

  arangodb::rest::SslInterface::sslHEX(hash, 16, p, length);

  return AqlValue(&hex[0], 32);
}

/// @brief function SHA1
AqlValue Functions::Sha1(ExpressionContext*, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  // create sha1
  char hash[21];
  char* p = &hash[0];
  size_t length;

  arangodb::rest::SslInterface::sslSHA1(buffer->c_str(), buffer->length(), p, length);

  // as hex
  char hex[41];
  p = &hex[0];

  arangodb::rest::SslInterface::sslHEX(hash, 20, p, length);

  return AqlValue(&hex[0], 40);
}

/// @brief function SHA512
AqlValue Functions::Sha512(ExpressionContext*, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  // create sha512
  char hash[65];
  char* p = &hash[0];
  size_t length;

  arangodb::rest::SslInterface::sslSHA512(buffer->c_str(), buffer->length(), p, length);

  // as hex
  char hex[129];
  p = &hex[0];

  arangodb::rest::SslInterface::sslHEX(hash, 64, p, length);

  return AqlValue(&hex[0], 128);
}

/// @brief function Crc32
AqlValue Functions::Crc32(ExpressionContext*, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  uint32_t crc = TRI_Crc32HashPointer(buffer->c_str(), buffer->length());
  char out[9];
  size_t length = TRI_StringUInt32HexInPlace(crc, &out[0]);
  return AqlValue(&out[0], length);
}

/// @brief function Fnv64
AqlValue Functions::Fnv64(ExpressionContext*, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  ::appendAsString(trx, adapter, value);

  uint64_t hashval = TRI_FnvHashPointer(buffer->c_str(), buffer->length());
  char out[17];
  size_t length = TRI_StringUInt64HexInPlace(hashval, &out[0]);
  return AqlValue(&out[0], length);
}

/// @brief function HASH
AqlValue Functions::Hash(ExpressionContext*, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  // throw away the top bytes so the hash value can safely be used
  // without precision loss when storing in JavaScript etc.
  uint64_t hash = value.hash() & 0x0007ffffffffffffULL;

  return AqlValue(AqlValueHintUInt(hash));
}

/// @brief function IS_KEY
AqlValue Functions::IsKey(ExpressionContext*, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (!value.isString()) {
    // not a string, so no valid key
    return AqlValue(AqlValueHintBool(false));
  }

  VPackValueLength l;
  char const* p = value.slice().getStringUnchecked(l);
  return AqlValue(AqlValueHintBool(KeyGenerator::validateKey(p, l)));
}

/// @brief function COUNT_DISTINCT
AqlValue Functions::CountDistinct(ExpressionContext* expressionContext,
                                  transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "COUNT_DISTINCT";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  auto options = trx->transactionContextPtr()->getVPackOptions();
  std::unordered_set<VPackSlice, arangodb::basics::VelocyPackHelper::VPackHash, arangodb::basics::VelocyPackHelper::VPackEqual>
      values(512, arangodb::basics::VelocyPackHelper::VPackHash(),
             arangodb::basics::VelocyPackHelper::VPackEqual(options));

  for (VPackSlice s : VPackArrayIterator(slice)) {
    if (!s.isNone()) {
      values.emplace(s.resolveExternal());
    }
  }

  return AqlValue(AqlValueHintUInt(values.size()));
}

/// @brief function UNIQUE
AqlValue Functions::Unique(ExpressionContext* expressionContext, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "UNIQUE";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  auto options = trx->transactionContextPtr()->getVPackOptions();
  std::unordered_set<VPackSlice, arangodb::basics::VelocyPackHelper::VPackHash, arangodb::basics::VelocyPackHelper::VPackEqual>
      values(512, arangodb::basics::VelocyPackHelper::VPackHash(),
             arangodb::basics::VelocyPackHelper::VPackEqual(options));

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  for (VPackSlice s : VPackArrayIterator(slice)) {
    if (s.isNone()) {
      continue;
    }

    s = s.resolveExternal();

    if (values.emplace(s).second) {
      builder->add(s);
    }
  }

  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function SORTED_UNIQUE
AqlValue Functions::SortedUnique(ExpressionContext* expressionContext,
                                 transaction::Methods* trx,
                                 VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "SORTED_UNIQUE";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  arangodb::basics::VelocyPackHelper::VPackLess<true> less(
      trx->transactionContext()->getVPackOptions(), &slice, &slice);
  std::set<VPackSlice, arangodb::basics::VelocyPackHelper::VPackLess<true>> values(less);
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (!it.isNone()) {
      values.insert(it);
    }
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : values) {
    builder->add(it);
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function SORTED
AqlValue Functions::Sorted(ExpressionContext* expressionContext, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "SORTED";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  arangodb::basics::VelocyPackHelper::VPackLess<true> less(
      trx->transactionContext()->getVPackOptions(), &slice, &slice);
  std::map<VPackSlice, size_t, arangodb::basics::VelocyPackHelper::VPackLess<true>> values(less);
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (!it.isNone()) {
      auto [itr, emplaced] = values.try_emplace(it, 1);
      if (!emplaced) {
        ++itr->second;
      }
    }
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : values) {
    for (size_t i = 0; i < it.second; ++i) {
      builder->add(it.first);
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function UNION
AqlValue Functions::Union(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  static char const* AFN = "UNION";

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  size_t const n = parameters.size();
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& value = extractFunctionParameterValue(parameters, i);

    if (!value.isArray()) {
      // not an array
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    TRI_IF_FAILURE("AqlFunctions::OutOfMemory1") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    AqlValueMaterializer materializer(trx);
    VPackSlice slice = materializer.slice(value, false);

    // this passes ownership for the JSON contents into result
    for (VPackSlice it : VPackArrayIterator(slice)) {
      builder->add(it);
      TRI_IF_FAILURE("AqlFunctions::OutOfMemory2") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }
    }
  }
  builder->close();
  TRI_IF_FAILURE("AqlFunctions::OutOfMemory3") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function UNION_DISTINCT
AqlValue Functions::UnionDistinct(ExpressionContext* expressionContext,
                                  transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters) {
  static char const* AFN = "UNION_DISTINCT";

  size_t const n = parameters.size();

  auto options = trx->transactionContextPtr()->getVPackOptions();
  std::unordered_set<VPackSlice, arangodb::basics::VelocyPackHelper::VPackHash, arangodb::basics::VelocyPackHelper::VPackEqual>
      values(512, arangodb::basics::VelocyPackHelper::VPackHash(),
             arangodb::basics::VelocyPackHelper::VPackEqual(options));

  std::vector<AqlValueMaterializer> materializers;
  materializers.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& value = extractFunctionParameterValue(parameters, i);

    if (!value.isArray()) {
      // not an array
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    materializers.emplace_back(trx);
    VPackSlice slice = materializers.back().slice(value, false);

    for (VPackSlice v : VPackArrayIterator(slice)) {
      v = v.resolveExternal();
      if (values.find(v) == values.end()) {
        TRI_IF_FAILURE("AqlFunctions::OutOfMemory1") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }

        values.emplace(v);
      }
    }
  }

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory2") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : values) {
    builder->add(it);
  }
  builder->close();

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory3") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function INTERSECTION
AqlValue Functions::Intersection(ExpressionContext* expressionContext,
                                 transaction::Methods* trx,
                                 VPackFunctionParameters const& parameters) {
  static char const* AFN = "INTERSECTION";

  auto options = trx->transactionContextPtr()->getVPackOptions();
  std::unordered_map<VPackSlice, size_t, arangodb::basics::VelocyPackHelper::VPackHash,
                     arangodb::basics::VelocyPackHelper::VPackEqual>
      values(512, arangodb::basics::VelocyPackHelper::VPackHash(),
             arangodb::basics::VelocyPackHelper::VPackEqual(options));

  size_t const n = parameters.size();
  std::vector<AqlValueMaterializer> materializers;
  materializers.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& value = extractFunctionParameterValue(parameters, i);

    if (!value.isArray()) {
      // not an array
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
      return AqlValue(AqlValueHintNull());
    }

    materializers.emplace_back(trx);
    VPackSlice slice = materializers.back().slice(value, false);

    for (VPackSlice it : VPackArrayIterator(slice)) {
      if (i == 0) {
        // round one

        TRI_IF_FAILURE("AqlFunctions::OutOfMemory1") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }

        values.try_emplace(it, 1);
      } else {
        // check if we have seen the same element before
        auto found = values.find(it);
        if (found != values.end()) {
          // already seen
          if ((*found).second < i) {
            (*found).second = 0;
          } else {
            (*found).second = i + 1;
          }
        }
      }
    }
  }

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory2") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : values) {
    if (it.second == n) {
      builder->add(it.first);
    }
  }
  builder->close();

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory3") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function JACCARD
AqlValue Functions::Jaccard(ExpressionContext* ctx, transaction::Methods* trx,
                            VPackFunctionParameters const& args) {
  static char const* AFN = "JACCARD";

  typedef std::unordered_map<VPackSlice, size_t, basics::VelocyPackHelper::VPackHash, basics::VelocyPackHelper::VPackEqual> ValuesMap;

  auto options = trx->transactionContextPtr()->getVPackOptions();
  ValuesMap values(512, basics::VelocyPackHelper::VPackHash(),
                   basics::VelocyPackHelper::VPackEqual(options));

  AqlValue const& lhs = extractFunctionParameterValue(args, 0);

  if (ADB_UNLIKELY(!lhs.isArray())) {
    // not an array
    registerWarning(ctx, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& rhs = extractFunctionParameterValue(args, 1);

  if (ADB_UNLIKELY(!rhs.isArray())) {
    // not an array
    registerWarning(ctx, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer lhsMaterializer(options);
  AqlValueMaterializer rhsMaterializer(options);

  VPackSlice lhsSlice = lhsMaterializer.slice(lhs, false);
  VPackSlice rhsSlice = rhsMaterializer.slice(rhs, false);

  size_t cardinality = 0;  // cardinality of intersection

  for (VPackSlice slice : VPackArrayIterator(lhsSlice)) {
    values.try_emplace(slice, 1);
  }

  for (VPackSlice slice : VPackArrayIterator(rhsSlice)) {
    auto& count = values.try_emplace(slice, 0).first->second;
    cardinality += count;
    count = 0;
  }

  auto const jaccard = values.empty() ? 1.0 : double_t(cardinality) / values.size();

  return AqlValue{AqlValueHintDouble{jaccard}};
}

/// @brief function OUTERSECTION
AqlValue Functions::Outersection(ExpressionContext* expressionContext,
                                 transaction::Methods* trx,
                                 VPackFunctionParameters const& parameters) {
  static char const* AFN = "OUTERSECTION";

  auto options = trx->transactionContextPtr()->getVPackOptions();
  std::unordered_map<VPackSlice, size_t, arangodb::basics::VelocyPackHelper::VPackHash,
                     arangodb::basics::VelocyPackHelper::VPackEqual>
      values(512, arangodb::basics::VelocyPackHelper::VPackHash(),
             arangodb::basics::VelocyPackHelper::VPackEqual(options));

  size_t const n = parameters.size();
  std::vector<AqlValueMaterializer> materializers;
  materializers.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& value = extractFunctionParameterValue(parameters, i);

    if (!value.isArray()) {
      // not an array
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
      return AqlValue(AqlValueHintNull());
    }

    materializers.emplace_back(trx);
    VPackSlice slice = materializers.back().slice(value, false);

    for (VPackSlice it : VPackArrayIterator(slice)) {
      // check if we have seen the same element before
      auto result = values.insert({it, 1});
      if (!result.second) {
        // already seen
        TRI_ASSERT(result.first->second > 0);
        ++(result.first->second);
      }
    }
  }

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory2") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : values) {
    if (it.second == 1) {
      builder->add(it.first);
    }
  }
  builder->close();

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory3") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function DISTANCE
AqlValue Functions::Distance(ExpressionContext* expressionContext, transaction::Methods*,
                             VPackFunctionParameters const& parameters) {
  static char const* AFN = "DISTANCE";

  AqlValue const& lat1 = extractFunctionParameterValue(parameters, 0);
  AqlValue const& lon1 = extractFunctionParameterValue(parameters, 1);
  AqlValue const& lat2 = extractFunctionParameterValue(parameters, 2);
  AqlValue const& lon2 = extractFunctionParameterValue(parameters, 3);

  // non-numeric input...
  if (!lat1.isNumber() || !lon1.isNumber() || !lat2.isNumber() || !lon2.isNumber()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  bool failed;
  bool error = false;
  double lat1Value = lat1.toDouble(failed);
  error |= failed;
  double lon1Value = lon1.toDouble(failed);
  error |= failed;
  double lat2Value = lat2.toDouble(failed);
  error |= failed;
  double lon2Value = lon2.toDouble(failed);
  error |= failed;

  if (error) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  auto toRadians = [](double degrees) -> double {
    return degrees * (std::acos(-1.0) / 180.0);
  };

  double p1 = toRadians(lat1Value);
  double p2 = toRadians(lat2Value);
  double d1 = toRadians(lat2Value - lat1Value);
  double d2 = toRadians(lon2Value - lon1Value);

  double a = std::sin(d1 / 2.0) * std::sin(d1 / 2.0) +
             std::cos(p1) * std::cos(p2) * std::sin(d2 / 2.0) * std::sin(d2 / 2.0);

  double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
  double const EARTHRADIAN = 6371000.0;  // metres

  return ::numberValue(EARTHRADIAN * c, true);
}

/// @brief function GEO_DISTANCE
AqlValue Functions::GeoDistance(ExpressionContext* expressionContext,
                                transaction::Methods* trx,
                                VPackFunctionParameters const& parameters) {
  TRI_ASSERT(trx);
  constexpr char const AFN[] = "GEO_DISTANCE";
  geo::ShapeContainer shape1, shape2;

  auto res = ::parseShape(*trx, extractFunctionParameterValue(parameters, 0), shape1);

  if (res.fail()) {
    registerWarning(expressionContext, AFN, res);
    return AqlValue(AqlValueHintNull());
  }

  res = ::parseShape(*trx, extractFunctionParameterValue(parameters, 1), shape2);

  if (res.fail()) {
    registerWarning(expressionContext, AFN, res);
    return AqlValue(AqlValueHintNull());
  }

  if (parameters.size() > 2 && parameters[2].isString()) {
    VPackValueLength len;
    const char* ptr = parameters[2].slice().getStringUnchecked(len);
    geo::Ellipsoid const& e = geo::utils::ellipsoidFromString(ptr, len);
    return ::numberValue(shape1.distanceFromCentroid(shape2.centroid(), e), true);
  }
  return ::numberValue(shape1.distanceFromCentroid(shape2.centroid()), true);
}

/// @brief function GEO_IN_RANGE
AqlValue Functions::GeoInRange(ExpressionContext* ctx,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& args) {
  TRI_ASSERT(trx);
  constexpr char const AFN[] = "GEO_IN_RANGE";

  auto const argc = args.size();

  if (argc < 4 || argc > 7) {
    registerWarning(ctx, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  geo::ShapeContainer shape1, shape2;
  auto res = parseShape(*trx, extractFunctionParameterValue(args, 0), shape1);

  if (res.fail()) {
    registerWarning(ctx, AFN, res);
    return AqlValue(AqlValueHintNull());
  }

  res = parseShape(*trx, extractFunctionParameterValue(args, 1), shape2);

  if (res.fail()) {
    registerWarning(ctx, AFN, res);
    return AqlValue(AqlValueHintNull());
  }

  auto const& lowerBound = extractFunctionParameterValue(args, 2);

  if (!lowerBound.isNumber()) {
    registerWarning(ctx, AFN, {TRI_ERROR_BAD_PARAMETER, "3rd argument requires a number"});
    return AqlValue(AqlValueHintNull());
  }

  auto const& upperBound = extractFunctionParameterValue(args, 3);

  if (!upperBound.isNumber()) {
    registerWarning(ctx, AFN, {TRI_ERROR_BAD_PARAMETER, "4th argument requires a number"});
    return AqlValue(AqlValueHintNull());
  }

  bool includeLower = true;
  bool includeUpper = true;
  geo::Ellipsoid const* ellipsoid = &geo::SPHERE;

  if (argc > 4) {
    auto const& includeLowerValue = extractFunctionParameterValue(args, 4);

    if (!includeLowerValue.isBoolean()) {
      registerWarning(ctx, AFN, {TRI_ERROR_BAD_PARAMETER, "5th argument requires a bool"});
      return AqlValue(AqlValueHintNull());
    }

    includeLower = includeLowerValue.toBoolean();

    if (argc > 5) {
      auto const& includeUpperValue = extractFunctionParameterValue(args, 4);

      if (!includeUpperValue.isBoolean()) {
        registerWarning(ctx, AFN, {TRI_ERROR_BAD_PARAMETER, "6th argument requires a bool"});
        return AqlValue(AqlValueHintNull());
      }

      includeUpper = includeUpperValue.toBoolean();
    }

    if (argc > 6) {
      auto const& value = extractFunctionParameterValue(args, 6);
      if (value.isString()) {
        VPackValueLength len;
        char const* ptr = value.slice().getStringUnchecked(len);
        ellipsoid = &geo::utils::ellipsoidFromString(ptr, len);
      }
    }
  }

  auto const minDistance = lowerBound.toDouble();
  auto const maxDistance = upperBound.toDouble();
  auto const distance = (ellipsoid == &geo::SPHERE
    ? shape1.distanceFromCentroid(shape2.centroid())
    : shape1.distanceFromCentroid(shape2.centroid(), *ellipsoid));

  return AqlValue{AqlValueHintBool{
    (includeLower ? distance >= minDistance
                  : distance > minDistance) &&
    (includeUpper ? distance <= maxDistance
                  : distance < maxDistance) }};
}

/// @brief function GEO_CONTAINS
AqlValue Functions::GeoContains(ExpressionContext* expressionContext,
                                transaction::Methods* trx,
                                VPackFunctionParameters const& parameters) {
  return ::geoContainsIntersect(expressionContext, trx, parameters,
                                "GEO_CONTAINS", true);
}

/// @brief function GEO_INTERSECTS
AqlValue Functions::GeoIntersects(ExpressionContext* expressionContext,
                                  transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters) {
  return ::geoContainsIntersect(expressionContext, trx, parameters,
                                "GEO_INTERSECTS", false);
}

/// @brief function GEO_EQUALS
AqlValue Functions::GeoEquals(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  AqlValue p1 = extractFunctionParameterValue(parameters, 0);
  AqlValue p2 = extractFunctionParameterValue(parameters, 1);

  if (!p1.isObject() || !p2.isObject()) {
    registerWarning(expressionContext, "GEO_EQUALS",
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "Expecting GeoJSON object"));
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer mat1(trx);
  AqlValueMaterializer mat2(trx);

  geo::ShapeContainer first, second;
  Result res1 = geo::geojson::parseRegion(mat1.slice(p1, true), first);
  Result res2 = geo::geojson::parseRegion(mat2.slice(p2, true), second);

  if (res1.fail()) {
    registerWarning(expressionContext, "GEO_EQUALS", res1);
    return AqlValue(AqlValueHintNull());
  }
  if (res2.fail()) {
    registerWarning(expressionContext, "GEO_EQUALS", res2);
    return AqlValue(AqlValueHintNull());
  }

  bool result = first.equals(&second);
  return AqlValue(AqlValueHintBool(result));
}

/// @brief function GEO_AREA
AqlValue Functions::GeoArea(ExpressionContext* expressionContext, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  AqlValue p1 = extractFunctionParameterValue(parameters, 0);
  AqlValue p2 = extractFunctionParameterValue(parameters, 1);

  AqlValueMaterializer mat(trx);

  geo::ShapeContainer shape;
  Result res = geo::geojson::parseRegion(mat.slice(p1, true), shape);

  if (res.fail()) {
    registerWarning(expressionContext, "GEO_AREA", res);
    return AqlValue(AqlValueHintNull());
  }

  auto detEllipsoid = [](AqlValue const& p) {
    if (p.isString()) {
      VPackValueLength len;
      const char* ptr = p.slice().getStringUnchecked(len);
      return geo::utils::ellipsoidFromString(ptr, len);
    }
    return geo::SPHERE;
  };
  return AqlValue(AqlValueHintDouble(shape.area(detEllipsoid(p2))));
}

/// @brief function IS_IN_POLYGON
AqlValue Functions::IsInPolygon(ExpressionContext* expressionContext,
                                transaction::Methods* trx,
                                VPackFunctionParameters const& parameters) {
  AqlValue const& coords = extractFunctionParameterValue(parameters, 0);
  AqlValue p2 = extractFunctionParameterValue(parameters, 1);
  AqlValue p3 = extractFunctionParameterValue(parameters, 2);

  if (!coords.isArray()) {
    registerWarning(expressionContext, "IS_IN_POLYGON", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  double latitude, longitude;
  bool geoJson = false;
  if (p2.isArray()) {
    if (p2.length() < 2) {
      registerInvalidArgumentWarning(expressionContext, "IS_IN_POLYGON");
      return AqlValue(AqlValueHintNull());
    }
    AqlValueMaterializer materializer(trx);
    VPackSlice arr = materializer.slice(p2, false);
    geoJson = p3.isBoolean() && p3.toBoolean();
    // if geoJson, map [lon, lat] -> lat, lon
    VPackSlice lat = geoJson ? arr[1] : arr[0];
    VPackSlice lon = geoJson ? arr[0] : arr[1];
    if (!lat.isNumber() || !lon.isNumber()) {
      registerInvalidArgumentWarning(expressionContext, "IS_IN_POLYGON");
      return AqlValue(AqlValueHintNull());
    }
    latitude = lat.getNumber<double>();
    longitude = lon.getNumber<double>();
  } else if (p2.isNumber() && p3.isNumber()) {
    bool failed1 = false, failed2 = false;
    latitude = p2.toDouble(failed1);
    longitude = p3.toDouble(failed2);
    if (failed1 || failed2) {
      registerInvalidArgumentWarning(expressionContext, "IS_IN_POLYGON");
      return AqlValue(AqlValueHintNull());
    }
  } else {
    registerInvalidArgumentWarning(expressionContext, "IS_IN_POLYGON");
    return AqlValue(AqlValueHintNull());
  }

  S2Loop loop;
  loop.set_s2debug_override(S2Debug::DISABLE);
  Result res = geo::geojson::parseLoop(coords.slice(), geoJson, loop);
  if (res.fail() || !loop.IsValid()) {
    registerWarning(expressionContext, "IS_IN_POLYGON", res);
    return AqlValue(AqlValueHintNull());
  }

  S2LatLng latLng = S2LatLng::FromDegrees(latitude, longitude);
  return AqlValue(AqlValueHintBool(loop.Contains(latLng.ToPoint())));
}

/// @brief geo constructors

/// @brief function GEO_POINT
AqlValue Functions::GeoPoint(ExpressionContext* expressionContext, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();

  if (n < 2) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue lon1 = extractFunctionParameterValue(parameters, 0);
  AqlValue lat1 = extractFunctionParameterValue(parameters, 1);

  // non-numeric input
  if (!lat1.isNumber() || !lon1.isNumber()) {
    registerWarning(expressionContext, "GEO_POINT",
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  bool failed;
  bool error = false;
  double lon1Value = lon1.toDouble(failed);
  error |= failed;
  double lat1Value = lat1.toDouble(failed);
  error |= failed;

  if (error) {
    registerWarning(expressionContext, "GEO_POINT",
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openObject();
  builder->add("type", VPackValue("Point"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));
  builder->add(VPackValue(lon1Value));
  builder->add(VPackValue(lat1Value));
  builder->close();
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function GEO_MULTIPOINT
AqlValue Functions::GeoMultiPoint(ExpressionContext* expressionContext,
                                  transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& geoArray = extractFunctionParameterValue(parameters, 0);

  if (!geoArray.isArray()) {
    registerWarning(expressionContext, "GEO_MULTIPOINT", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }
  if (geoArray.length() < 2) {
    registerWarning(expressionContext, "GEO_MULTIPOINT",
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "a MultiPoint needs at least two positions"));
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);

  builder->openObject();
  builder->add("type", VPackValue("MultiPoint"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));

  AqlValueMaterializer materializer(trx);
  VPackSlice s = materializer.slice(geoArray, false);
  for (VPackSlice v : VPackArrayIterator(s)) {
    if (v.isArray()) {
      builder->openArray();
      for (auto const& coord : VPackArrayIterator(v)) {
        if (coord.isNumber()) {
          builder->add(VPackValue(coord.getNumber<double>()));
        } else {
          registerWarning(expressionContext, "GEO_MULTIPOINT",
                          Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                                 "not a numeric value"));
          return AqlValue(AqlValueHintNull());
        }
      }
      builder->close();
    } else {
      registerWarning(expressionContext, "GEO_MULTIPOINT",
                      Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                             "not an array containing positions"));
      return AqlValue(AqlValueHintNull());
    }
  }

  builder->close();
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function GEO_POLYGON
AqlValue Functions::GeoPolygon(ExpressionContext* expressionContext,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& geoArray = extractFunctionParameterValue(parameters, 0);

  if (!geoArray.isArray()) {
    registerWarning(expressionContext, "GEO_POLYGON", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openObject();
  builder->add("type", VPackValue("Polygon"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));

  AqlValueMaterializer materializer(trx);
  VPackSlice s = materializer.slice(geoArray, false);

  Result res = ::parseGeoPolygon(s, *builder.get());
  if (res.fail()) {
    registerWarning(expressionContext, "GEO_POLYGON", res);
    return AqlValue(AqlValueHintNull());
  }

  builder->close();  // coordinates
  builder->close();  // object

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function GEO_MULTIPOLYGON
AqlValue Functions::GeoMultiPolygon(ExpressionContext* expressionContext,
                                    transaction::Methods* trx,
                                    VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& geoArray = extractFunctionParameterValue(parameters, 0);

  if (!geoArray.isArray()) {
    registerWarning(expressionContext, "GEO_MULTIPOLYGON", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice s = materializer.slice(geoArray, false);

  /*
  return GEO_MULTIPOLYGON([
    [
      [[40, 40], [20, 45], [45, 30], [40, 40]]
    ],
    [
      [[20, 35], [10, 30], [10, 10], [30, 5], [45, 20], [20, 35]],
      [[30, 20], [20, 15], [20, 25], [30, 20]]
    ]
  ])
  */

  TRI_ASSERT(s.isArray());
  if (s.length() < 2) {
    registerWarning(
        expressionContext, "GEO_MULTIPOLYGON",
        Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
               "a MultiPolygon needs at least two Polygons inside."));
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openObject();
  builder->add("type", VPackValue("MultiPolygon"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));

  for (auto const& arrayOfPolygons : VPackArrayIterator(s)) {
    if (!arrayOfPolygons.isArray()) {
      registerWarning(
          expressionContext, "GEO_MULTIPOLYGON",
          Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                 "a MultiPolygon needs at least two Polygons inside."));
      return AqlValue(AqlValueHintNull());
    }
    builder->openArray();  // arrayOfPolygons
    for (VPackSlice v : VPackArrayIterator(arrayOfPolygons)) {
      Result res = ::parseGeoPolygon(v, *builder.get());
      if (res.fail()) {
        registerWarning(expressionContext, "GEO_MULTIPOLYGON", res);
        return AqlValue(AqlValueHintNull());
      }
    }
    builder->close();  // arrayOfPolygons close
  }

  builder->close();
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function GEO_LINESTRING
AqlValue Functions::GeoLinestring(ExpressionContext* expressionContext,
                                  transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& geoArray = extractFunctionParameterValue(parameters, 0);

  if (!geoArray.isArray()) {
    registerWarning(expressionContext, "GEO_LINESTRING", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }
  if (geoArray.length() < 2) {
    registerWarning(expressionContext, "GEO_LINESTRING",
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "a LineString needs at least two positions"));
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);

  builder->add(VPackValue(VPackValueType::Object));
  builder->add("type", VPackValue("LineString"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));

  AqlValueMaterializer materializer(trx);
  VPackSlice s = materializer.slice(geoArray, false);
  for (VPackSlice v : VPackArrayIterator(s)) {
    if (v.isArray()) {
      builder->openArray();
      for (auto const& coord : VPackArrayIterator(v)) {
        if (coord.isNumber()) {
          builder->add(VPackValue(coord.getNumber<double>()));
        } else {
          registerWarning(expressionContext, "GEO_LINESTRING",
                          Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                                 "not a numeric value"));
          return AqlValue(AqlValueHintNull());
        }
      }
      builder->close();
    } else {
      registerWarning(expressionContext, "GEO_LINESTRING",
                      Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                             "not an array containing positions"));
      return AqlValue(AqlValueHintNull());
    }
  }

  builder->close();
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function GEO_MULTILINESTRING
AqlValue Functions::GeoMultiLinestring(ExpressionContext* expressionContext,
                                       transaction::Methods* trx,
                                       VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& geoArray = extractFunctionParameterValue(parameters, 0);

  if (!geoArray.isArray()) {
    registerWarning(expressionContext, "GEO_MULTILINESTRING", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }
  if (geoArray.length() < 1) {
    registerWarning(
        expressionContext, "GEO_MULTILINESTRING",
        Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
               "a MultiLineString needs at least one array of linestrings"));
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);

  builder->add(VPackValue(VPackValueType::Object));
  builder->add("type", VPackValue("MultiLineString"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));

  AqlValueMaterializer materializer(trx);
  VPackSlice s = materializer.slice(geoArray, false);
  for (VPackSlice v : VPackArrayIterator(s)) {
    if (v.isArray()) {
      if (v.length() > 1) {
        builder->openArray();
        for (auto const& inner : VPackArrayIterator(v)) {
          if (inner.isArray()) {
            builder->openArray();
            for (auto const& coord : VPackArrayIterator(inner)) {
              if (coord.isNumber()) {
                builder->add(VPackValue(coord.getNumber<double>()));
              } else {
                registerWarning(expressionContext, "GEO_MULTILINESTRING",
                                Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                                       "not a numeric value"));
                return AqlValue(AqlValueHintNull());
              }
            }
            builder->close();
          } else {
            registerWarning(expressionContext, "GEO_MULTILINESTRING",
                            Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                                   "not an array containing positions"));
            return AqlValue(AqlValueHintNull());
          }
        }
        builder->close();
      } else {
        registerWarning(expressionContext, "GEO_MULTILINESTRING",
                        Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                               "not an array containing linestrings"));
        return AqlValue(AqlValueHintNull());
      }
    } else {
      registerWarning(expressionContext, "GEO_MULTILINESTRING",
                      Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                             "not an array containing positions"));
      return AqlValue(AqlValueHintNull());
    }
  }

  builder->close();
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function FLATTEN
AqlValue Functions::Flatten(ExpressionContext* expressionContext, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "FLATTEN";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);
  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  size_t maxDepth = 1;
  if (parameters.size() == 2) {
    AqlValue const& maxDepthValue = extractFunctionParameterValue(parameters, 1);
    bool failed;
    double tmpMaxDepth = maxDepthValue.toDouble(failed);
    if (failed || tmpMaxDepth < 1) {
      maxDepth = 1;
    } else {
      maxDepth = static_cast<size_t>(tmpMaxDepth);
    }
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice listSlice = materializer.slice(list, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  ::flattenList(listSlice, maxDepth, 0, *builder.get());
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function ZIP
AqlValue Functions::Zip(ExpressionContext* expressionContext, transaction::Methods* trx,
                        VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "ZIP";

  AqlValue const& keys = extractFunctionParameterValue(parameters, 0);
  AqlValue const& values = extractFunctionParameterValue(parameters, 1);

  if (!keys.isArray() || !values.isArray() || keys.length() != values.length()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer keyMaterializer(trx);
  VPackSlice keysSlice = keyMaterializer.slice(keys, false);

  AqlValueMaterializer valueMaterializer(trx);
  VPackSlice valuesSlice = valueMaterializer.slice(values, false);

  transaction::BuilderLeaser builder(trx);
  builder->openObject();

  // Buffer will temporarily hold the keys
  std::unordered_set<std::string> keysSeen;
  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  VPackArrayIterator keysIt(keysSlice);
  VPackArrayIterator valuesIt(valuesSlice);

  TRI_ASSERT(keysIt.size() == valuesIt.size());

  while (keysIt.valid()) {
    TRI_ASSERT(valuesIt.valid());

    // stringify key
    buffer->reset();
    Stringify(trx, adapter, keysIt.value());

    if (keysSeen.emplace(buffer->c_str(), buffer->length()).second) {
      // non-duplicate key
      builder->add(buffer->c_str(), buffer->length(), valuesIt.value());
    }

    keysIt.next();
    valuesIt.next();
  }

  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function JSON_STRINGIFY
AqlValue Functions::JsonStringify(ExpressionContext*, transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  transaction::StringBufferLeaser buffer(trx);
  arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

  VPackDumper dumper(&adapter, trx->transactionContextPtr()->getVPackOptions());
  dumper.dump(slice);

  return AqlValue(buffer->begin(), buffer->length());
}

/// @brief function JSON_PARSE
AqlValue Functions::JsonParse(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  static char const* AFN = "JSON_PARSE";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  if (!slice.isString()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  VPackValueLength l;
  char const* p = slice.getStringUnchecked(l);

  try {
    std::shared_ptr<VPackBuilder> builder = VPackParser::fromJson(p, l);
    return AqlValue(builder->slice(), builder->size());
  } catch (...) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }
}

/// @brief function PARSE_IDENTIFIER
AqlValue Functions::ParseIdentifier(ExpressionContext* expressionContext,
                                    transaction::Methods* trx,
                                    VPackFunctionParameters const& parameters) {
  static char const* AFN = "PARSE_IDENTIFIER";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  std::string identifier;
  if (value.isObject() && value.hasKey(StaticStrings::IdString)) {
    auto resolver = trx->resolver();
    TRI_ASSERT(resolver != nullptr);
    bool localMustDestroy;
    AqlValue valueStr =
        value.get(*resolver, StaticStrings::IdString, localMustDestroy, false);
    AqlValueGuard guard(valueStr, localMustDestroy);

    if (valueStr.isString()) {
      identifier = valueStr.slice().copyString();
    }
  } else if (value.isString()) {
    identifier = value.slice().copyString();
  }

  if (identifier.empty()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  size_t pos = identifier.find('/');
  if (pos == std::string::npos || identifier.find('/', pos + 1) != std::string::npos) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openObject();
  builder->add("collection", VPackValuePair(identifier.data(), pos, VPackValueType::String));
  builder->add("key", VPackValuePair(identifier.data() + pos + 1,
                                     identifier.size() - pos - 1, VPackValueType::String));
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function Slice
AqlValue Functions::Slice(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "SLICE";

  AqlValue const& baseArray = extractFunctionParameterValue(parameters, 0);

  if (!baseArray.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  // determine lower bound
  AqlValue fromValue = extractFunctionParameterValue(parameters, 1);
  int64_t from = fromValue.toInt64();
  if (from < 0) {
    from = baseArray.length() + from;
    if (from < 0) {
      from = 0;
    }
  }

  // determine upper bound
  AqlValue const& toValue = extractFunctionParameterValue(parameters, 2);
  int64_t to;
  if (toValue.isNull(true)) {
    to = baseArray.length();
  } else {
    to = toValue.toInt64();
    if (to >= 0) {
      to += from;
    } else {
      // negative to value
      to = baseArray.length() + to;
      if (to < 0) {
        to = 0;
      }
    }
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice arraySlice = materializer.slice(baseArray, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  int64_t pos = 0;
  VPackArrayIterator it(arraySlice);
  while (it.valid()) {
    if (pos >= from && pos < to) {
      builder->add(it.value());
    }
    ++pos;
    if (pos >= to) {
      // done
      break;
    }
    it.next();
  }

  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function Minus
AqlValue Functions::Minus(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  static char const* AFN = "MINUS";

  AqlValue const& baseArray = extractFunctionParameterValue(parameters, 0);

  if (!baseArray.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  auto options = trx->transactionContextPtr()->getVPackOptions();
  std::unordered_map<VPackSlice, size_t, arangodb::basics::VelocyPackHelper::VPackHash,
                     arangodb::basics::VelocyPackHelper::VPackEqual>
      contains(512, arangodb::basics::VelocyPackHelper::VPackHash(),
               arangodb::basics::VelocyPackHelper::VPackEqual(options));

  // Fill the original map
  AqlValueMaterializer materializer(trx);
  VPackSlice arraySlice = materializer.slice(baseArray, false);

  VPackArrayIterator it(arraySlice);
  while (it.valid()) {
    contains.try_emplace(it.value(), it.index());
    it.next();
  }

  // Iterate through all following parameters and delete found elements from the
  // map
  for (size_t k = 1; k < parameters.size(); ++k) {
    AqlValue const& next = extractFunctionParameterValue(parameters, k);
    if (!next.isArray()) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }

    AqlValueMaterializer materializer(trx);
    VPackSlice arraySlice = materializer.slice(next, false);

    for (VPackSlice search : VPackArrayIterator(arraySlice)) {
      auto find = contains.find(search);

      if (find != contains.end()) {
        contains.erase(find);
      }
    }
  }

  // We omit the normalize part from js, cannot occur here
  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : contains) {
    builder->add(it.first);
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function Document
AqlValue Functions::Document(ExpressionContext* expressionContext, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "DOCUMENT";

  if (parameters.size() == 1) {
    AqlValue const& id = extractFunctionParameterValue(parameters, 0);
    transaction::BuilderLeaser builder(trx);
    if (id.isString()) {
      std::string identifier(id.slice().copyString());
      std::string colName;
      ::getDocumentByIdentifier(trx, colName, identifier, true, *builder.get());
      if (builder->isEmpty()) {
        // not found
        return AqlValue(AqlValueHintNull());
      }
      return AqlValue(builder->slice(), builder->size());
    }
    if (id.isArray()) {
      AqlValueMaterializer materializer(trx);
      VPackSlice idSlice = materializer.slice(id, false);
      builder->openArray();
      for (auto const& next : VPackArrayIterator(idSlice)) {
        if (next.isString()) {
          std::string identifier = next.copyString();
          std::string colName;
          ::getDocumentByIdentifier(trx, colName, identifier, true, *builder.get());
        }
      }
      builder->close();
      return AqlValue(builder->slice(), builder->size());
    }
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& collectionValue = extractFunctionParameterValue(parameters, 0);
  if (!collectionValue.isString()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }
  std::string collectionName(collectionValue.slice().copyString());

  AqlValue const& id = extractFunctionParameterValue(parameters, 1);
  if (id.isString()) {
    transaction::BuilderLeaser builder(trx);
    std::string identifier(id.slice().copyString());
    ::getDocumentByIdentifier(trx, collectionName, identifier, true, *builder.get());
    if (builder->isEmpty()) {
      return AqlValue(AqlValueHintNull());
    }
    return AqlValue(builder->slice(), builder->size());
  }

  if (id.isArray()) {
    transaction::BuilderLeaser builder(trx);
    builder->openArray();

    AqlValueMaterializer materializer(trx);
    VPackSlice idSlice = materializer.slice(id, false);
    for (auto const& next : VPackArrayIterator(idSlice)) {
      if (next.isString()) {
        std::string identifier(next.copyString());
        ::getDocumentByIdentifier(trx, collectionName, identifier, true,
                                  *builder.get());
      }
    }

    builder->close();
    return AqlValue(builder->slice(), builder->size());
  }

  // Id has invalid format
  return AqlValue(AqlValueHintNull());
}

/// @brief function MATCHES
AqlValue Functions::Matches(ExpressionContext* expressionContext, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  static char const* AFN = "MATCHES";

  AqlValue const& docToFind = extractFunctionParameterValue(parameters, 0);

  if (!docToFind.isObject()) {
    return AqlValue(AqlValueHintBool(false));
  }

  AqlValue const& exampleDocs = extractFunctionParameterValue(parameters, 1);

  bool retIdx = false;
  if (parameters.size() == 3) {
    retIdx = extractFunctionParameterValue(parameters, 2).toBoolean();
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice const docSlice = materializer.slice(docToFind, true);

  TRI_ASSERT(docSlice.isObject());

  transaction::BuilderLeaser builder(trx);
  AqlValueMaterializer exampleMaterializer(trx);
  VPackSlice examples = exampleMaterializer.slice(exampleDocs, false);

  if (!examples.isArray()) {
    builder->openArray();
    builder->add(examples);
    builder->close();
    examples = builder->slice();
  }

  auto options = trx->transactionContextPtr()->getVPackOptions();

  bool foundMatch;
  int32_t idx = -1;

  for (auto const& example : VPackArrayIterator(examples)) {
    idx++;

    if (!example.isObject()) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      continue;
    }

    foundMatch = true;

    TRI_ASSERT(example.isObject());
    TRI_ASSERT(docSlice.isObject());
    for (auto it : VPackObjectIterator(example, true)) {
      VPackSlice keySlice = docSlice.get(it.key.stringRef());

      if (it.value.isNull() && keySlice.isNone()) {
        continue;
      }

      if (keySlice.isNone() ||
          // compare inner content
          !basics::VelocyPackHelper::equal(keySlice, it.value, false, options,
                                           &docSlice, &example)) {
        foundMatch = false;
        break;
      }
    }

    if (foundMatch) {
      if (retIdx) {
        return AqlValue(AqlValueHintInt(idx));
      } else {
        return AqlValue(AqlValueHintBool(true));
      }
    }
  }

  if (retIdx) {
    return AqlValue(AqlValueHintInt(-1));
  }

  return AqlValue(AqlValueHintBool(false));
}

/// @brief function ROUND
AqlValue Functions::Round(ExpressionContext*, transaction::Methods*,
                          VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();

  // Rounds down for < x.4999 and up for > x.50000
  return ::numberValue(std::floor(input + 0.5), true);
}

/// @brief function ABS
AqlValue Functions::Abs(ExpressionContext*, transaction::Methods*,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::abs(input), true);
}

/// @brief function CEIL
AqlValue Functions::Ceil(ExpressionContext*, transaction::Methods*,
                         VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::ceil(input), true);
}

/// @brief function FLOOR
AqlValue Functions::Floor(ExpressionContext*, transaction::Methods*,
                          VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::floor(input), true);
}

/// @brief function SQRT
AqlValue Functions::Sqrt(ExpressionContext*, transaction::Methods*,
                         VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::sqrt(input), true);
}

/// @brief function POW
AqlValue Functions::Pow(ExpressionContext*, transaction::Methods*,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& baseValue = extractFunctionParameterValue(parameters, 0);
  AqlValue const& expValue = extractFunctionParameterValue(parameters, 1);

  double base = baseValue.toDouble();
  double exp = expValue.toDouble();

  return ::numberValue(std::pow(base, exp), true);
}

/// @brief function LOG
AqlValue Functions::Log(ExpressionContext*, transaction::Methods*,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::log(input), true);
}

/// @brief function LOG2
AqlValue Functions::Log2(ExpressionContext*, transaction::Methods*,
                         VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::log2(input), true);
}

/// @brief function LOG10
AqlValue Functions::Log10(ExpressionContext*, transaction::Methods*,
                          VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::log10(input), true);
}

/// @brief function EXP
AqlValue Functions::Exp(ExpressionContext*, transaction::Methods*,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::exp(input), true);
}

/// @brief function EXP2
AqlValue Functions::Exp2(ExpressionContext*, transaction::Methods*,
                         VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::exp2(input), true);
}

/// @brief function SIN
AqlValue Functions::Sin(ExpressionContext*, transaction::Methods*,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::sin(input), true);
}

/// @brief function COS
AqlValue Functions::Cos(ExpressionContext*, transaction::Methods*,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::cos(input), true);
}

/// @brief function TAN
AqlValue Functions::Tan(ExpressionContext*, transaction::Methods*,
                        VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::tan(input), true);
}

/// @brief function ASIN
AqlValue Functions::Asin(ExpressionContext*, transaction::Methods*,
                         VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::asin(input), true);
}

/// @brief function ACOS
AqlValue Functions::Acos(ExpressionContext*, transaction::Methods*,
                         VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::acos(input), true);
}

/// @brief function ATAN
AqlValue Functions::Atan(ExpressionContext*, transaction::Methods*,
                         VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::atan(input), true);
}

/// @brief function ATAN2
AqlValue Functions::Atan2(ExpressionContext*, transaction::Methods*,
                          VPackFunctionParameters const& parameters) {
  AqlValue value1 = extractFunctionParameterValue(parameters, 0);
  AqlValue value2 = extractFunctionParameterValue(parameters, 1);

  double input1 = value1.toDouble();
  double input2 = value2.toDouble();
  return ::numberValue(std::atan2(input1, input2), true);
}

/// @brief function RADIANS
AqlValue Functions::Radians(ExpressionContext*, transaction::Methods*,
                            VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double degrees = value.toDouble();
  // acos(-1) == PI
  return ::numberValue(degrees * (std::acos(-1.0) / 180.0), true);
}

/// @brief function DEGREES
AqlValue Functions::Degrees(ExpressionContext*, transaction::Methods*,
                            VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double radians = value.toDouble();
  // acos(-1) == PI
  return ::numberValue(radians * (180.0 / std::acos(-1.0)), true);
}

/// @brief function PI
AqlValue Functions::Pi(ExpressionContext*, transaction::Methods* trx,
                       VPackFunctionParameters const& parameters) {
  // acos(-1) == PI
  return ::numberValue(std::acos(-1.0), true);
}

/// @brief function RAND
AqlValue Functions::Rand(ExpressionContext*, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  // This random functionality is not too good yet...
  return ::numberValue(static_cast<double>(std::rand()) / RAND_MAX, true);
}

/// @brief function FIRST_DOCUMENT
AqlValue Functions::FirstDocument(ExpressionContext*, transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& a = extractFunctionParameterValue(parameters, i);
    if (a.isObject()) {
      return a.clone();
    }
  }

  return AqlValue(AqlValueHintNull());
}

/// @brief function FIRST_LIST
AqlValue Functions::FirstList(ExpressionContext*, transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& a = extractFunctionParameterValue(parameters, i);
    if (a.isArray()) {
      return a.clone();
    }
  }

  return AqlValue(AqlValueHintNull());
}

/// @brief function PUSH
AqlValue Functions::Push(ExpressionContext* expressionContext, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "PUSH";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);
  AqlValue const& toPush = extractFunctionParameterValue(parameters, 1);

  AqlValueMaterializer toPushMaterializer(trx);
  VPackSlice p = toPushMaterializer.slice(toPush, false);

  if (list.isNull(true)) {
    transaction::BuilderLeaser builder(trx);
    builder->openArray();
    builder->add(p);
    builder->close();
    return AqlValue(builder->slice(), builder->size());
  }

  if (!list.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  AqlValueMaterializer materializer(trx);
  VPackSlice l = materializer.slice(list, false);

  for (VPackSlice it : VPackArrayIterator(l)) {
    builder->add(it);
  }
  if (parameters.size() == 3) {
    auto options = trx->transactionContextPtr()->getVPackOptions();
    AqlValue const& unique = extractFunctionParameterValue(parameters, 2);
    if (!unique.toBoolean() || !::listContainsElement(options, l, p)) {
      builder->add(p);
    }
  } else {
    builder->add(p);
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function POP
AqlValue Functions::Pop(ExpressionContext* expressionContext, transaction::Methods* trx,
                        VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "POP";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (list.isNull(true)) {
    return AqlValue(AqlValueHintNull());
  }

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(list, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  auto iterator = VPackArrayIterator(slice);
  while (iterator.valid() && !iterator.isLast()) {
    builder->add(iterator.value());
    iterator.next();
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function APPEND
AqlValue Functions::Append(ExpressionContext* expressionContext, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "APPEND";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);
  AqlValue const& toAppend = extractFunctionParameterValue(parameters, 1);

  if (toAppend.isNull(true)) {
    return list.clone();
  }

  AqlValueMaterializer toAppendMaterializer(trx);
  VPackSlice t = toAppendMaterializer.slice(toAppend, false);

  if (t.isArray() && t.length() == 0) {
    return list.clone();
  }

  bool unique = false;
  if (parameters.size() == 3) {
    AqlValue const& a = extractFunctionParameterValue(parameters, 2);
    unique = a.toBoolean();
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice l = materializer.slice(list, false);

  if (l.isNull()) {
    return toAppend.clone();
  }

  if (!l.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  auto options = trx->transactionContextPtr()->getVPackOptions();
  std::unordered_set<VPackSlice, basics::VelocyPackHelper::VPackHash, basics::VelocyPackHelper::VPackEqual> added(
      11, basics::VelocyPackHelper::VPackHash(),
      basics::VelocyPackHelper::VPackEqual(options));

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  for (VPackSlice it : VPackArrayIterator(l)) {
    if (!unique || added.insert(it).second) {
      builder->add(it);
    }
  }

  AqlValueMaterializer materializer2(trx);
  VPackSlice slice = materializer2.slice(toAppend, false);

  if (!slice.isArray()) {
    if (!unique || added.find(slice) == added.end()) {
      builder->add(slice);
    }
  } else {
    for (VPackSlice it : VPackArrayIterator(slice)) {
      if (!unique || added.insert(it).second) {
        builder->add(it);
      }
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function UNSHIFT
AqlValue Functions::Unshift(ExpressionContext* expressionContext, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "UNSHIFT";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isNull(true) && !list.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& toAppend = extractFunctionParameterValue(parameters, 1);
  bool unique = false;
  if (parameters.size() == 3) {
    AqlValue const& a = extractFunctionParameterValue(parameters, 2);
    unique = a.toBoolean();
  }

  auto options = trx->transactionContextPtr()->getVPackOptions();
  size_t unused;
  if (unique && list.isArray() &&
      ::listContainsElement(trx, options, list, toAppend, unused)) {
    // Short circuit, nothing to do return list
    return list.clone();
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice a = materializer.slice(toAppend, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  builder->add(a);

  if (list.isArray()) {
    AqlValueMaterializer listMaterializer(trx);
    VPackSlice v = listMaterializer.slice(list, false);
    for (VPackSlice it : VPackArrayIterator(v)) {
      builder->add(it);
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function SHIFT
AqlValue Functions::Shift(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "SHIFT";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);
  if (list.isNull(true)) {
    return AqlValue(AqlValueHintNull());
  }

  if (!list.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  if (list.length() > 0) {
    AqlValueMaterializer materializer(trx);
    VPackSlice l = materializer.slice(list, false);

    auto iterator = VPackArrayIterator(l);
    // This jumps over the first element
    iterator.next();
    while (iterator.valid()) {
      builder->add(iterator.value());
      iterator.next();
    }
  }
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function REMOVE_VALUE
AqlValue Functions::RemoveValue(ExpressionContext* expressionContext,
                                transaction::Methods* trx,
                                VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "REMOVE_VALUE";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (list.isNull(true)) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  if (!list.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  auto options = trx->transactionContextPtr()->getVPackOptions();

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  bool useLimit = false;
  int64_t limit = list.length();

  if (parameters.size() == 3) {
    AqlValue const& limitValue = extractFunctionParameterValue(parameters, 2);
    if (!limitValue.isNull(true)) {
      limit = limitValue.toInt64();
      useLimit = true;
    }
  }

  AqlValue const& toRemove = extractFunctionParameterValue(parameters, 1);
  AqlValueMaterializer toRemoveMaterializer(trx);
  VPackSlice r = toRemoveMaterializer.slice(toRemove, false);

  AqlValueMaterializer materializer(trx);
  VPackSlice v = materializer.slice(list, false);

  for (VPackSlice it : VPackArrayIterator(v)) {
    if (useLimit && limit == 0) {
      // Just copy
      builder->add(it);
      continue;
    }
    if (arangodb::basics::VelocyPackHelper::equal(r, it, false, options)) {
      --limit;
      continue;
    }
    builder->add(it);
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function REMOVE_VALUES
AqlValue Functions::RemoveValues(ExpressionContext* expressionContext,
                                 transaction::Methods* trx,
                                 VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "REMOVE_VALUES";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);
  AqlValue const& values = extractFunctionParameterValue(parameters, 1);

  if (values.isNull(true)) {
    return list.clone();
  }

  if (list.isNull(true)) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  if (!list.isArray() || !values.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  auto options = trx->transactionContextPtr()->getVPackOptions();
  AqlValueMaterializer valuesMaterializer(trx);
  VPackSlice v = valuesMaterializer.slice(values, false);

  AqlValueMaterializer listMaterializer(trx);
  VPackSlice l = listMaterializer.slice(list, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (VPackSlice it : VPackArrayIterator(l)) {
    if (!::listContainsElement(options, v, it)) {
      builder->add(it);
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function REMOVE_NTH
AqlValue Functions::RemoveNth(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "REMOVE_NTH";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (list.isNull(true)) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  if (!list.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  double const count = static_cast<double>(list.length());
  AqlValue const& position = extractFunctionParameterValue(parameters, 1);
  double p = position.toDouble();
  if (p >= count || p < -count) {
    // out of bounds
    return list.clone();
  }

  if (p < 0) {
    p += count;
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice v = materializer.slice(list, false);

  transaction::BuilderLeaser builder(trx);
  size_t target = static_cast<size_t>(p);
  size_t cur = 0;
  builder->openArray();
  for (VPackSlice it : VPackArrayIterator(v)) {
    if (cur != target) {
      builder->add(it);
    }
    cur++;
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function ReplaceNth
AqlValue Functions::ReplaceNth(ExpressionContext* expressionContext,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "REPLACE_NTH";

  AqlValue const& baseArray = extractFunctionParameterValue(parameters, 0);
  AqlValue const& offset = extractFunctionParameterValue(parameters, 1);
  AqlValue const& newValue = extractFunctionParameterValue(parameters, 2);
  AqlValue const& paddValue = extractFunctionParameterValue(parameters, 3);

  bool havePadValue = parameters.size() == 4;

  if (!baseArray.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  if (offset.isNull(true)) {
    THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH, AFN);
  }
  auto length = baseArray.length();
  uint64_t replaceOffset;
  int64_t posParam = offset.toInt64();
  if (posParam >= 0) {
    replaceOffset = static_cast<uint64_t>(posParam);
  } else {
    replaceOffset = (static_cast<int64_t>(length) + posParam < 0)
                        ? 0
                        : static_cast<uint64_t>(length + posParam);
  }

  if (length < replaceOffset && !havePadValue) {
    THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH, AFN);
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice arraySlice = materializer.slice(baseArray, false);
  VPackSlice replaceValue = materializer.slice(newValue, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  VPackArrayIterator it(arraySlice);
  while (it.valid()) {
    if (it.index() != replaceOffset) {
      builder->add(it.value());
    } else {
      builder->add(replaceValue);
    }
    it.next();
  }

  uint64_t pos = length;
  if (replaceOffset >= length) {
    VPackSlice paddVpValue = materializer.slice(paddValue, false);
    while (pos < replaceOffset) {
      builder->add(paddVpValue);
      ++pos;
    }
    builder->add(replaceValue);
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function NOT_NULL
AqlValue Functions::NotNull(ExpressionContext*, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  size_t const n = parameters.size();
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& element = extractFunctionParameterValue(parameters, i);
    if (!element.isNull(true)) {
      return element.clone();
    }
  }
  return AqlValue(AqlValueHintNull());
}

/// @brief function CURRENT_DATABASE
AqlValue Functions::CurrentDatabase(ExpressionContext* expressionContext,
                                    transaction::Methods* trx,
                                    VPackFunctionParameters const& parameters) {
  return AqlValue(expressionContext->vocbase().name());
}

/// @brief function CURRENT_USER
AqlValue Functions::CurrentUser(ExpressionContext*, transaction::Methods* trx,
                                VPackFunctionParameters const& parameters) {
  std::string const& username = ExecContext::current().user();
  if (username.empty()) {
    return AqlValue(AqlValueHintNull());
  }
  return AqlValue(username);
}

/// @brief function COLLECTION_COUNT
AqlValue Functions::CollectionCount(ExpressionContext*, transaction::Methods* trx,
                                    VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "COLLECTION_COUNT";

  AqlValue const& element = extractFunctionParameterValue(parameters, 0);
  if (!element.isString()) {
    THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH, AFN);
  }

  TRI_ASSERT(ServerState::instance()->isSingleServerOrCoordinator());
  std::string const collectionName = element.slice().copyString();
  OperationOptions options(ExecContext::current());
  OperationResult res = trx->count(collectionName, transaction::CountType::Normal, options);
  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res.result);
  }

  return AqlValue(res.slice());
}

/// @brief function CHECK_DOCUMENT
AqlValue Functions::CheckDocument(ExpressionContext*, transaction::Methods* trx,
                                  VPackFunctionParameters const& parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (!value.isObject()) {
    // no document at all
    return AqlValue(AqlValueHintBool(false));
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice slice = materializer.slice(value, false);

  return AqlValue(AqlValueHintBool(::isValidDocument(slice)));
}

/// @brief function VARIANCE_SAMPLE
AqlValue Functions::VarianceSample(ExpressionContext* expressionContext,
                                   transaction::Methods* trx,
                                   VPackFunctionParameters const& parameters) {
  static char const* AFN = "VARIANCE_SAMPLE";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  double value = 0.0;
  size_t count = 0;

  if (!::variance(trx, list, value, count)) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (count < 2) {
    return AqlValue(AqlValueHintNull());
  }

  return ::numberValue(value / (count - 1), true);
}

/// @brief function VARIANCE_POPULATION
AqlValue Functions::VariancePopulation(ExpressionContext* expressionContext,
                                       transaction::Methods* trx,
                                       VPackFunctionParameters const& parameters) {
  static char const* AFN = "VARIANCE_POPULATION";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  double value = 0.0;
  size_t count = 0;

  if (!::variance(trx, list, value, count)) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (count < 1) {
    return AqlValue(AqlValueHintNull());
  }

  return ::numberValue(value / count, true);
}

/// @brief function STDDEV_SAMPLE
AqlValue Functions::StdDevSample(ExpressionContext* expressionContext,
                                 transaction::Methods* trx,
                                 VPackFunctionParameters const& parameters) {
  static char const* AFN = "STDDEV_SAMPLE";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  double value = 0.0;
  size_t count = 0;

  if (!::variance(trx, list, value, count)) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (count < 2) {
    return AqlValue(AqlValueHintNull());
  }

  return ::numberValue(std::sqrt(value / (count - 1)), true);
}

/// @brief function STDDEV_POPULATION
AqlValue Functions::StdDevPopulation(ExpressionContext* expressionContext,
                                     transaction::Methods* trx,
                                     VPackFunctionParameters const& parameters) {
  static char const* AFN = "STDDEV_POPULATION";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  double value = 0.0;
  size_t count = 0;

  if (!::variance(trx, list, value, count)) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (count < 1) {
    return AqlValue(AqlValueHintNull());
  }

  return ::numberValue(std::sqrt(value / count), true);
}

/// @brief function MEDIAN
AqlValue Functions::Median(ExpressionContext* expressionContext, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  static char const* AFN = "MEDIAN";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  std::vector<double> values;
  if (!::sortNumberList(trx, list, values)) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (values.empty()) {
    return AqlValue(AqlValueHintNull());
  }
  size_t const l = values.size();
  size_t midpoint = l / 2;

  if (l % 2 == 0) {
    return ::numberValue((values[midpoint - 1] + values[midpoint]) / 2, true);
  }
  return ::numberValue(values[midpoint], true);
}

/// @brief function PERCENTILE
AqlValue Functions::Percentile(ExpressionContext* expressionContext,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& parameters) {
  static char const* AFN = "PERCENTILE";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& border = extractFunctionParameterValue(parameters, 1);

  if (!border.isNumber()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  double p = border.toDouble();
  if (p <= 0.0 || p > 100.0) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  bool useInterpolation = false;

  if (parameters.size() == 3) {
    AqlValue const& methodValue = extractFunctionParameterValue(parameters, 2);
    if (!methodValue.isString()) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
    std::string method = methodValue.slice().copyString();
    if (method == "interpolation") {
      useInterpolation = true;
    } else if (method == "rank") {
      useInterpolation = false;
    } else {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
  }

  std::vector<double> values;
  if (!::sortNumberList(trx, list, values)) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (values.empty()) {
    return AqlValue(AqlValueHintNull());
  }

  size_t l = values.size();
  if (l == 1) {
    return ::numberValue(values[0], true);
  }

  TRI_ASSERT(l > 1);

  if (useInterpolation) {
    double const idx = p * (l + 1) / 100.0;
    double const pos = floor(idx);

    if (pos >= l) {
      return ::numberValue(values[l - 1], true);
    }
    if (pos <= 0) {
      return AqlValue(AqlValueHintNull());
    }

    double const delta = idx - pos;
    return ::numberValue(delta * (values[static_cast<size_t>(pos)] -
                                  values[static_cast<size_t>(pos) - 1]) +
                             values[static_cast<size_t>(pos) - 1],
                         true);
  }

  double const idx = p * l / 100.0;
  double const pos = ceil(idx);
  if (pos >= l) {
    return ::numberValue(values[l - 1], true);
  }
  if (pos <= 0) {
    return AqlValue(AqlValueHintNull());
  }

  return ::numberValue(values[static_cast<size_t>(pos) - 1], true);
}

/// @brief function RANGE
AqlValue Functions::Range(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "RANGE";

  AqlValue const& left = extractFunctionParameterValue(parameters, 0);
  AqlValue const& right = extractFunctionParameterValue(parameters, 1);

  double from = left.toDouble();
  double to = right.toDouble();

  if (parameters.size() < 3) {
    return AqlValue(left.toInt64(), right.toInt64());
  }

  AqlValue const& stepValue = extractFunctionParameterValue(parameters, 2);
  if (stepValue.isNull(true)) {
    // no step specified. return a real range object
    return AqlValue(left.toInt64(), right.toInt64());
  }

  double step = stepValue.toDouble();

  if (step == 0.0 || (from < to && step < 0.0) || (from > to && step > 0.0)) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray(true);
  if (step < 0.0 && to <= from) {
    TRI_ASSERT(step != 0.0);
    Range::throwIfTooBigForMaterialization(static_cast<uint64_t>((from - to) / -step));
    for (; from >= to; from += step) {
      builder->add(VPackValue(from));
    }
  } else {
    TRI_ASSERT(step != 0.0);
    Range::throwIfTooBigForMaterialization(static_cast<uint64_t>((to - from) / step));
    for (; from <= to; from += step) {
      builder->add(VPackValue(from));
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function POSITION
AqlValue Functions::Position(ExpressionContext* expressionContext, transaction::Methods* trx,
                             VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "POSITION";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  bool returnIndex = false;
  if (parameters.size() == 3) {
    AqlValue const& a = extractFunctionParameterValue(parameters, 2);
    returnIndex = a.toBoolean();
  }

  if (list.length() > 0) {
    AqlValue const& searchValue = extractFunctionParameterValue(parameters, 1);
    auto options = trx->transactionContextPtr()->getVPackOptions();

    size_t index;
    if (::listContainsElement(trx, options, list, searchValue, index)) {
      if (!returnIndex) {
        // return true
        return AqlValue(AqlValueHintBool(true));
      }
      // return position
      return AqlValue(AqlValueHintUInt(index));
    }
  }

  // not found
  if (!returnIndex) {
    // return false
    return AqlValue(AqlValueHintBool(false));
  }

  // return -1
  return AqlValue(AqlValueHintInt(-1));
}

/// @brief function CALL
AqlValue Functions::Call(ExpressionContext* expressionContext, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  static char const* AFN = "CALL";

  AqlValue const& invokeFN = extractFunctionParameterValue(parameters, 0);
  if (!invokeFN.isString()) {
    registerError(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  ::arangodb::containers::SmallVector<AqlValue>::allocator_type::arena_type arena;
  VPackFunctionParameters invokeParams{arena};
  if (parameters.size() >= 2) {
    // we have a list of parameters, need to copy them over except the
    // functionname:
    invokeParams.reserve(parameters.size() - 1);

    for (uint64_t i = 1; i < parameters.size(); i++) {
      invokeParams.push_back(extractFunctionParameterValue(parameters, i));
    }
  }

  return ::callApplyBackend(expressionContext, trx, AFN, invokeFN, invokeParams);
}

/// @brief function APPLY
AqlValue Functions::Apply(ExpressionContext* expressionContext, transaction::Methods* trx,
                          VPackFunctionParameters const& parameters) {
  static char const* AFN = "APPLY";

  AqlValue const& invokeFN = extractFunctionParameterValue(parameters, 0);
  if (!invokeFN.isString()) {
    registerError(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  ::arangodb::containers::SmallVector<AqlValue>::allocator_type::arena_type arena;
  VPackFunctionParameters invokeParams{arena};
  AqlValue rawParamArray;
  std::vector<bool> mustFree;

  auto guard = scopeGuard([&mustFree, &invokeParams]() {
    for (size_t i = 0; i < mustFree.size(); ++i) {
      if (mustFree[i]) {
        invokeParams[i].destroy();
      }
    }
  });

  if (parameters.size() == 2) {
    // We have a parameter that should be an array, whichs content we need to
    // make the sub functions parameters.
    rawParamArray = extractFunctionParameterValue(parameters, 1);

    if (!rawParamArray.isArray()) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
    uint64_t len = rawParamArray.length();
    invokeParams.reserve(len);
    mustFree.reserve(len);
    for (uint64_t i = 0; i < len; i++) {
      bool f;
      invokeParams.push_back(rawParamArray.at(i, f, false));
      mustFree.push_back(f);
    }
  }

  return ::callApplyBackend(expressionContext, trx, AFN, invokeFN, invokeParams);
}

/// @brief function VERSION
AqlValue Functions::Version(ExpressionContext* expressionContext, transaction::Methods* trx,
                            VPackFunctionParameters const& parameters) {
  return AqlValue(rest::Version::getServerVersion());
}

/// @brief function IS_SAME_COLLECTION
AqlValue Functions::IsSameCollection(ExpressionContext* expressionContext,
                                     transaction::Methods* trx,
                                     VPackFunctionParameters const& parameters) {
  static char const* AFN = "IS_SAME_COLLECTION";

  std::string const first = ::extractCollectionName(trx, parameters, 0);
  std::string const second = ::extractCollectionName(trx, parameters, 1);

  if (!first.empty() && !second.empty()) {
    return AqlValue(AqlValueHintBool(first == second));
  }

  registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
  return AqlValue(AqlValueHintNull());
}

AqlValue Functions::PregelResult(ExpressionContext* expressionContext,
                                 transaction::Methods*,
                                 VPackFunctionParameters const& parameters) {
  static char const* AFN = "PREGEL_RESULT";

  AqlValue arg1 = extractFunctionParameterValue(parameters, 0);
  if (!arg1.isNumber()) {
    THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH, AFN);
  }
  bool withId = false;
  AqlValue arg2 = extractFunctionParameterValue(parameters, 1);
  if (arg2.isBoolean()) {
    withId = arg2.slice().getBool();
  }

  uint64_t execNr = arg1.toInt64();
  std::shared_ptr<pregel::PregelFeature> feature = pregel::PregelFeature::instance();
  if (!feature) {
    registerWarning(expressionContext, AFN, TRI_ERROR_FAILED);
    return AqlValue(AqlValueHintEmptyArray());
  }

  VPackBuffer<uint8_t> buffer;
  VPackBuilder builder(buffer);
  if (ServerState::instance()->isCoordinator()) {
    std::shared_ptr<pregel::Conductor> c = feature->conductor(execNr);
    if (!c) {
      registerWarning(expressionContext, AFN, TRI_ERROR_HTTP_NOT_FOUND);
      return AqlValue(AqlValueHintEmptyArray());
    }
    c->collectAQLResults(builder, withId);

  } else {
    std::shared_ptr<pregel::IWorker> worker = feature->worker(execNr);
    if (!worker) {
      registerWarning(expressionContext, AFN, TRI_ERROR_HTTP_NOT_FOUND);
      return AqlValue(AqlValueHintEmptyArray());
    }
    worker->aqlResult(builder, withId);
  }

  if (builder.isEmpty()) {
    return AqlValue(AqlValueHintEmptyArray());
  }
  TRI_ASSERT(builder.slice().isArray());

  // move the buffer into
  return AqlValue(std::move(buffer));
}

AqlValue Functions::Assert(ExpressionContext* expressionContext, transaction::Methods* trx,
                           VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "ASSERT";

  auto const expr = extractFunctionParameterValue(parameters, 0);
  auto const message = extractFunctionParameterValue(parameters, 1);

  if (!message.isString()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }
  if (!expr.toBoolean()) {
    std::string msg = message.slice().copyString();
    expressionContext->registerError(TRI_ERROR_QUERY_USER_ASSERT, msg.data());
  }
  return AqlValue(AqlValueHintBool(true));
}

AqlValue Functions::Warn(ExpressionContext* expressionContext, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "WARN";

  auto const expr = extractFunctionParameterValue(parameters, 0);
  auto const message = extractFunctionParameterValue(parameters, 1);

  if (!message.isString()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  if (!expr.toBoolean()) {
    std::string msg = message.slice().copyString();
    expressionContext->registerWarning(TRI_ERROR_QUERY_USER_WARN, msg.data());
    return AqlValue(AqlValueHintBool(false));
  }
  return AqlValue(AqlValueHintBool(true));
}

AqlValue Functions::Fail(ExpressionContext*, transaction::Methods* trx,
                         VPackFunctionParameters const& parameters) {
  if (parameters.size() == 0) {
    THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FAIL_CALLED, "");
  }

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isString()) {
    THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FAIL_CALLED, "");
  }

  AqlValueMaterializer materializer(trx);
  VPackSlice str = materializer.slice(value, false);
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_FAIL_CALLED, str.copyString());
}

/// @brief function DATE_FORMAT
AqlValue Functions::DateFormat(ExpressionContext* expressionContext,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& params) {
  static char const* AFN = "DATE_FORMAT";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, params, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& aqlFormatString = extractFunctionParameterValue(params, 1);
  if (!aqlFormatString.isString()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(arangodb::basics::formatDate(aqlFormatString.slice().copyString(), tp));
}

/// @brief function DECODE_REV
AqlValue Functions::DecodeRev(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  auto const rev = extractFunctionParameterValue(parameters, 0);
  if (!rev.isString()) {
    registerInvalidArgumentWarning(expressionContext, "DECODE_REV");
    return AqlValue(AqlValueHintNull());
  }

  VPackValueLength l;
  char const* p = rev.slice().getString(l);
  uint64_t revInt = arangodb::basics::HybridLogicalClock::decodeTimeStamp(p, l);

  if (revInt == 0 || revInt == UINT64_MAX) {
    registerInvalidArgumentWarning(expressionContext, "DECODE_REV");
    return AqlValue(AqlValueHintNull());
  }

  uint64_t timeMilli = arangodb::basics::HybridLogicalClock::extractTime(revInt);
  uint64_t count = arangodb::basics::HybridLogicalClock::extractCount(revInt);
  time_t timeSeconds = timeMilli / 1000;
  uint64_t millis = timeMilli % 1000;
  struct tm date;
  TRI_gmtime(timeSeconds, &date);

  char buffer[32];
  strftime(buffer, 32, "%Y-%m-%dT%H:%M:%S.000Z", &date);
  // fill millisecond part not covered by strftime
  buffer[20] = static_cast<char>(millis / 100) + '0';
  buffer[21] = ((millis / 10) % 10) + '0';
  buffer[22] = (millis % 10) + '0';
  // buffer[23] is 'Z'
  buffer[24] = 0;

  transaction::BuilderLeaser builder(trx);
  builder->openObject();
  builder->add("date", VPackValue(buffer));
  builder->add("count", VPackValue(count));
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

AqlValue Functions::SchemaGet(ExpressionContext* expressionContext,
                              transaction::Methods* trx,
                              VPackFunctionParameters const& parameters) {
  // SCHEMA_GET(collectionName) -> schema object
  std::string const collectionName = ::extractCollectionName(trx, parameters, 0);

  if (collectionName.empty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "could not extract collection name from parameters");
  }

  std::shared_ptr<arangodb::LogicalCollection> logicalCollection;
  methods::Collections::lookup(trx->vocbase(), collectionName, logicalCollection);
  if (!logicalCollection) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                                   "could not find collection: " + collectionName);
  }

  transaction::BuilderLeaser builder(trx);
  logicalCollection->schemaToVelocyPack(*builder.get());
  VPackSlice slice = builder->slice();

  if (!slice.isObject()) {
    return AqlValue(AqlValueHintNull{});
  }

  auto ruleSlice = slice.get(StaticStrings::ValidationParameterRule);

  if (!ruleSlice.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "schema definition for collection " +
                                       collectionName + " has no rule object");
  }

  return AqlValue(slice, builder->size());
}

AqlValue Functions::SchemaValidate(ExpressionContext* expressionContext,
                                   transaction::Methods* trx,
                                   VPackFunctionParameters const& parameters) {
  // SCHEMA_VALIDATE(doc, schema object) -> { valid (bool), [errorMessage (string)] }
  static char const* AFN = "SCHEMA_VALIDATE";
  auto const* vpackOptions = trx->transactionContext()->getVPackOptions();

  AqlValue const& docValue = extractFunctionParameterValue(parameters, 0);
  AqlValue const& schemaValue = extractFunctionParameterValue(parameters, 1);

  if (!docValue.isObject()) {
    registerWarning(expressionContext, AFN,
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "expecting document object"));
    return AqlValue(AqlValueHintNull());
  }

  if (schemaValue.isNull(false) || 
      (schemaValue.isObject() && schemaValue.length() == 0)) {
    // schema is null or {}
    transaction::BuilderLeaser resultBuilder(trx);
    {
      VPackObjectBuilder guard(resultBuilder.builder());
      resultBuilder->add("valid", VPackValue(true));
    }
    return AqlValue(resultBuilder->slice(), resultBuilder->size());
  }

  if (!schemaValue.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER, "second parameter is not a schema object: " +
                                     schemaValue.slice().toJson());
  }
  auto* validator = expressionContext->buildValidator(schemaValue.slice());

  // store and restore validation level this is cheaper than modifying the VPack
  auto storedLevel = validator->level();

  // override level so the validation will be executed no matter what
  validator->setLevel(ValidationLevel::Strict);

  Result res;
  {
    arangodb::ScopeGuard guardi([storedLevel, &validator]{
        validator->setLevel(storedLevel);
    });

    res = validator->validateOne(docValue.slice(), vpackOptions);
  }

  transaction::BuilderLeaser resultBuilder(trx);
  {
    VPackObjectBuilder guard(resultBuilder.builder());
    resultBuilder->add("valid", VPackValue(res.ok()));
    if (res.fail()) {
      resultBuilder->add(StaticStrings::ErrorMessage, VPackValue(res.errorMessage()));
    }
  }

  return AqlValue(resultBuilder->slice(), resultBuilder->size());
}

AqlValue Functions::Interleave(arangodb::aql::ExpressionContext* expressionContext,
                               transaction::Methods* trx,
                               VPackFunctionParameters const& parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "INTERLEAVE";

  struct ArrayIteratorPair {
    VPackArrayIterator current;
    VPackArrayIterator end;
  };

  std::list<ArrayIteratorPair> iters;
  std::vector<AqlValueMaterializer> materializers;
  materializers.reserve(parameters.size());

  for (AqlValue const& parameter : parameters) {
    auto& materializer = materializers.emplace_back(trx);
    VPackSlice slice = materializer.slice(parameter, true);

    if (!slice.isArray()) {
      // not an array
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
      return AqlValue(AqlValueHintNull());
    } else if (slice.isEmptyArray()) {
      continue;  // skip empty array here
    }

    VPackArrayIterator iter(slice);
    ArrayIteratorPair pair{iter.begin(), iter.end()};
    iters.emplace_back(pair);
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  while (!iters.empty()) {  // in this loop we only deal with nonempty arrays
    for (auto i = iters.begin(); i != iters.end();) {
      builder->add(i->current.value());  // thus this will always be valid on the first iteration
      i->current++;
      if (i->current == i->end) {
        i = iters.erase(i);
      } else {
        i++;
      }
    }
  }

  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

AqlValue Functions::NotImplemented(ExpressionContext* expressionContext,
                                   transaction::Methods*,
                                   VPackFunctionParameters const& params) {
  registerError(expressionContext, "UNKNOWN", TRI_ERROR_NOT_IMPLEMENTED);
  return AqlValue(AqlValueHintNull());
}

