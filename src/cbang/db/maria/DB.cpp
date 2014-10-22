/******************************************************************************\

          This file is part of the C! library.  A.K.A the cbang library.

              Copyright (c) 2003-2014, Cauldron Development LLC
                 Copyright (c) 2003-2014, Stanford University
                             All rights reserved.

        The C! library is free software: you can redistribute it and/or
        modify it under the terms of the GNU Lesser General Public License
        as published by the Free Software Foundation, either version 2.1 of
        the License, or (at your option) any later version.

        The C! library is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
        Lesser General Public License for more details.

        You should have received a copy of the GNU Lesser General Public
        License along with the C! library.  If not, see
        <http://www.gnu.org/licenses/>.

        In addition, BSD licensing may be granted on a case by case basis
        by written permission from at least one of the copyright holders.
        You may request written permission by emailing the authors.

                For information regarding this software email:
                               Joseph Coffland
                        joseph@cauldrondevelopment.com

\******************************************************************************/

#include "DB.h"

#include <cbang/String.h>
#include <cbang/Exception.h>
#include <cbang/time/Time.h>

#include <cbang/event/Base.h>
#include <cbang/event/Event.h>

#include <mysql/mysql.h>

#include <string.h>

#ifdef WIN32
#define STR_DATA(s) (s).c_str()
#else
#define STR_DATA(s) (s).data()
#endif

using namespace std;
using namespace cb;
using namespace cb::MariaDB;


namespace {
  int ready_to_mysql(DB::ready_t ready) {
    int x = 0;
    if (ready & DB::READY_READ) x |= MYSQL_WAIT_READ;
    if (ready & DB::READY_WRITE) x |= MYSQL_WAIT_WRITE;
    if (ready & DB::READY_TIMEOUT) x |= MYSQL_WAIT_TIMEOUT;
    return x;
  }
}


DB::DB(st_mysql *db) :
  db(db ? db : mysql_init(0)), nonBlocking(false), connected(false),
  stored(false), status(0), continueFunc(0) {
  if (!db) THROW("Failed to create MariaDB");
}


DB::~DB() {
  if (db) {
    mysql_close(db);
    free(db);
  }
}


void DB::setInitCommand(const string &cmd) {
  if (!mysql_options(db, MYSQL_INIT_COMMAND, cmd.c_str()))
    THROWS("Failed to set MariaDB init command: " << cmd);
}


void DB::enableCompression() {
  if (!mysql_options(db, MYSQL_OPT_COMPRESS, 0))
    THROW("Failed to enable MariaDB compress");
}


void DB::setConnectTimeout(unsigned secs) {
  if (!mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &secs))
    THROWS("Failed to set MariaDB connect timeout to " << secs);
}


void DB::setLocalInFile(bool enable) {
  if (!mysql_options(db, MYSQL_OPT_LOCAL_INFILE, enable ? "1" : 0))
    THROWS("Failed to " << (enable ? "enable" : "disable")
           << " MariaD local infile");
}


void DB::enableNamedPipe() {
  if (!mysql_options(db, MYSQL_OPT_NAMED_PIPE, 0))
    THROWS("Failed to enable MariaDB named pipe");
}


void DB::setProtocol(protocol_t protocol) {
  mysql_protocol_type type;
  switch (protocol) {
  case PROTOCOL_TCP: type = MYSQL_PROTOCOL_TCP; break;
  case PROTOCOL_SOCKET: type = MYSQL_PROTOCOL_SOCKET; break;
  case PROTOCOL_PIPE: type = MYSQL_PROTOCOL_PIPE; break;
  default: THROWS("Invalid protocol " << protocol);
  }

  if (!mysql_options(db, MYSQL_OPT_PROTOCOL, &type))
    THROWS("Failed to set MariaDB protocol to " << protocol);
}


void DB::setReconnect(bool enable) {
  if (!mysql_options(db, MYSQL_OPT_RECONNECT, enable ? "1" : 0))
    THROWS("Failed to " << (enable ? "enable" : "disable")
           << "MariaDB auto reconnect");
}


void DB::setReadTimeout(unsigned secs) {
  if (!mysql_options(db, MYSQL_OPT_READ_TIMEOUT, &secs))
    THROWS("Failed to set MariaDB read timeout to " << secs);
}


void DB::setWriteTimeout(unsigned secs) {
  if (!mysql_options(db, MYSQL_OPT_WRITE_TIMEOUT, &secs))
    THROWS("Failed to set MariaDB write timeout to " << secs);
}


void DB::setDefaultFile(const string &path) {
  if (!mysql_options(db, MYSQL_READ_DEFAULT_FILE, path.c_str()))
    THROWS("Failed to set MariaDB default type to " << path);
}


void DB::readDefaultGroup(const string &path) {
  if (!mysql_options(db, MYSQL_READ_DEFAULT_GROUP, path.c_str()))
    THROWS("Failed to read MariaDB default group file " << path);
}


void DB::setReportDataTruncation(bool enable) {
  if (!mysql_options(db, MYSQL_REPORT_DATA_TRUNCATION, enable ? "1" : 0))
    THROWS("Failed to" << (enable ? "enable" : "disable")
           << " MariaDB data truncation reporting.");
}


void DB::setCharacterSet(const string &name) {
  if (!mysql_options(db, MYSQL_SET_CHARSET_NAME, name.c_str()))
    THROWS("Failed to set MariaDB character set to " << name);
}


void DB::enableNonBlocking() {
  if (!mysql_options(db, MYSQL_OPT_NONBLOCK, 0))
    THROW("Failed to set MariaDB to non-blocking mode");
  nonBlocking = true;
}


void DB::connect(const string &host, const string &user, const string &password,
                 const string &dbName, unsigned port, const string &socketName,
                 flags_t flags) {
  assertNotPending();
  MYSQL *db = mysql_real_connect
    (this->db, host.c_str(), user.c_str(), password.c_str(), dbName.c_str(),
     port, socketName.c_str(), flags);

  if (!db) raiseError("Failed to connect");
  connected = true;
}


bool DB::connectNB(const string &host, const string &user,
                   const string &password, const string &dbName, unsigned port,
                   const string &socketName, flags_t flags) {
  assertNotPending();
  assertNonBlocking();

  MYSQL *db = 0;
  status = mysql_real_connect_start
    (&db, this->db, host.c_str(), user.c_str(), password.c_str(),
     dbName.c_str(), port, socketName.c_str(), flags);

  if (status) {
    continueFunc = &DB::connectContinue;
    return false;
  }

  if (!db) raiseError("Failed to connect");
  connected = true;

  return true;
}


void DB::close() {
  assertConnected();
  assertNotPending();
  assertDontHaveResult();

  mysql_close(db);

  connected = false;
}


bool DB::closeNB() {
  assertConnected();
  assertNotPending();
  assertNonBlocking();
  assertDontHaveResult();

  status = mysql_close_start(db);
  if (status) {
    continueFunc = &DB::closeContinue;
    return false;
  }

  connected = false;
  return true;
}


void DB::use(const string &dbName) {
  assertConnected();
  assertNotPending();

  if (mysql_select_db(db, dbName.c_str()))
    raiseError("Failed to select DB");
}


bool DB::useNB(const string &dbName) {
  assertConnected();
  assertNotPending();
  assertNonBlocking();

  int ret = 0;
  status = mysql_select_db_start(&ret, db, dbName.c_str());

  if (status) {
    continueFunc = &DB::useContinue;
    return false;
  }

  if (ret) raiseError("Failed to select DB");

  return true;
}


void DB::query(const string &s) {
  assertConnected();
  assertNotPending();

  if (mysql_real_query(db, STR_DATA(s), s.length()))
    raiseError("Query failed");
}


bool DB::queryNB(const string &s) {
  assertConnected();
  assertNotPending();
  assertNonBlocking();

  int ret = 0;
  status = mysql_real_query_start(&ret, db, STR_DATA(s), s.length());

  if (status) {
    continueFunc = &DB::queryContinue;
    return false;
  }

  if (ret) raiseError("Query failed");

  return true;
}


void DB::useResult() {
  assertConnected();
  assertNotPending();
  assertDontHaveResult();

  res = mysql_use_result(db);
  if (!res) THROW("Failed to use result");

  stored = false;
}


void DB::storeResult() {
  assertConnected();
  assertNotPending();
  assertDontHaveResult();

  res = mysql_store_result(db);
  if (!res) raiseError("Failed to store result");

  stored = true;
}


bool DB::storeResultNB() {
  assertConnected();
  assertNotPending();
  assertNonBlocking();
  assertDontHaveResult();

  status = mysql_store_result_start(&res, db);
  if (status) {
    continueFunc = &DB::storeResultContinue;
    return false;
  }

  if (!res) raiseError("Failed to store result");

  stored = true;

  return true;
}


bool DB::haveResult() const {
  return res;
}


bool DB::nextResult() {
  assertConnected();
  assertNotPending();

  int ret = mysql_next_result(db);
  if (0 < ret) raiseError("Failed get next result");

  return ret == 0;
}


bool DB::nextResultNB() {
  assertConnected();
  assertNotPending();
  assertNonBlocking();

  int ret = 0;
  status = mysql_next_result_start(&ret, db);
  if (status) {
    continueFunc = &DB::nextResultContinue;
    return false;
  }

  if (0 < ret) raiseError("Failed get next result");

  return true;
}


bool DB::moreResults() const {
  assertConnected();
  return mysql_more_results(db);
}


void DB::freeResult() {
  assertNotPending();
  assertHaveResult();
  mysql_free_result(res);
  res = 0;
}


bool DB::freeResultNB() {
  assertNotPending();
  assertNonBlocking();
  assertHaveResult();

  status = mysql_free_result_start(res);
  if (status) {
    continueFunc = &DB::freeResultContinue;
    return false;
  }

  res = 0;

  return true;
}


uint64_t DB::getRowCount() const {
  assertHaveResult();
  return mysql_num_rows(res);
}


unsigned DB::getFieldCount() const {
  assertHaveRow();
  return mysql_num_fields(res);
}


bool DB::fetchRow() {
  assertNotPending();
  assertHaveResult();
  return mysql_fetch_row(res);
}


bool DB::fetchRowNB() {
  assertNotPending();
  assertNonBlocking();
  assertHaveResult();

  MYSQL_ROW row = 0;
  status = mysql_fetch_row_start(&row, res);
  if (status) {
    continueFunc = &DB::fetchRowContinue;
    return false;
  }

  return true;
}


bool DB::haveRow() const {
  return res && res->current_row;
}


void DB::seekRow(uint64_t row) {
  assertHaveResult();
  if (!stored) THROW("Must use storeResult() before seekRow()");
  if (mysql_num_rows(res) <= row) THROWS("Row seek out of range " << row);
  mysql_data_seek(res, row);
}


Field DB::getField(unsigned i) const {
  assertInFieldRange(i);
  return &mysql_fetch_fields(res)[i];
}


Field::type_t DB::getType(unsigned i) const {
  return getField(i).getType();
}


unsigned DB::getLength(unsigned i) const {
  assertInFieldRange(i);
  return mysql_fetch_lengths(res)[i];
}


const char *DB::getData(unsigned i) const {
  assertInFieldRange(i);
  return res->current_row[i];
}


string DB::getString(unsigned i) const {
  unsigned length = getLength(i);
  return string(res->current_row[i], length);
}


double DB::getDouble(unsigned i) const {
  if (!getField(i).isNumber()) THROWS("Field " << i << " is not a number");
  return String::parseDouble(getString(i));
}


uint32_t DB::getU32(unsigned i) const {
  if (!getField(i).isInteger()) THROWS("Field " << i << " is not an integer");
  return String::parseU32(getString(i));
}


int32_t DB::getS32(unsigned i) const {
  if (!getField(i).isInteger()) THROWS("Field " << i << " is not an integer");
  return String::parseS32(getString(i));
}


uint64_t DB::getU64(unsigned i) const {
  if (!getField(i).isInteger()) THROWS("Field " << i << " is not an integer");
  return String::parseU64(getString(i));
}


int64_t DB::getS64(unsigned i) const {
  if (!getField(i).isInteger()) THROWS("Field " << i << " is not an integer");
  return String::parseS64(getString(i));
}


uint64_t DB::getBit(unsigned i) const {
  if (getType(i) != Field::TYPE_BIT) THROWS("Field " << i << " is not bit");

  uint64_t x = 0;
  for (const char *ptr = res->current_row[i]; *ptr; ptr++) {
    x <<= 1;
    if (*ptr == '1') x |= 1;
  }

  return x;
}


void DB::getSet(unsigned i, set<string> &s) const {
  if (getType(i) != Field::TYPE_SET) THROWS("Field " << i << " is not a set");

  const char *start = res->current_row[i];
  const char *end = start;

  while (*end) {
    if (*end == ',') {
      s.insert(string(start, end - start));
      start = end + 1;
    }
    end++;
  }
}


double DB::getTime(unsigned i) const {
  assertInFieldRange(i);

  char *s = res->current_row[i];
  unsigned len = res->lengths[i];

  // Parse decimal part
  double decimal = 0;
  char *ptr = strchr(s, '.');
  if (ptr) {
    decimal = String::parseDouble(ptr);
    len = ptr - s;
  }

  string time = string(s, len);

  switch (getType(i)) {
  case Field::TYPE_YEAR:
    if (len == 2) return decimal + Time::parse(time, "%y");
    return decimal + Time::parse(time, "%Y");

  case Field::TYPE_DATE:
    return decimal + Time::parse(time, "%Y-%m-%d");

  case Field::TYPE_TIME:
    return decimal + Time::parse(time, "%H%M%S");

  case Field::TYPE_TIMESTAMP:
  case Field::TYPE_DATETIME:
    return decimal + Time::parse(time, "%Y-%m-%d %H%M%S");

  default: THROWS("Invalid time type");
  }
}


string DB::getInfo() const {
  return mysql_info(db);
}


string DB::getError() const {
  return mysql_error(db);
}


void DB::raiseError(const string &msg) const {
  THROWS("MariaDB: " << msg << ": " << getError());
}


void DB::assertConnected() const {
  if (!connected) THROW("Not connected");
}


void DB::assertPending() const {
  if (!nonBlocking || !status) THROW("Connect not pending");
}


void DB::assertNotPending() const {
  if (status) raiseError("Non-blocking call still pending");
}


void DB::assertNonBlocking() const {
  if (!nonBlocking) raiseError("Connection is not in nonBlocking mode");
}


void DB::assertHaveResult() const {
  if (!haveResult()) raiseError("Don't have result, must call query() and "
                                "useResult() or storeResult()");
}


void DB::assertDontHaveResult() const {
  if (haveResult()) raiseError("Already have result, must call freeResult()");
}


void DB::assertHaveRow() const {
  if (!haveRow()) raiseError("Don't have row, must call fetchRow()");
}


void DB::assertInFieldRange(unsigned i) const {
  if (getFieldCount() <= 0) THROWS("Out of field range " << i);
}


bool DB::continueNB(ready_t ready) {
  assertPending();
  if (!continueFunc) THROWS("Continue function not set");
  return (this->*continueFunc)(ready);
}


bool DB::waitRead() const {
  return status & MYSQL_WAIT_READ;
}


bool DB::waitWrite() const {
  return status & MYSQL_WAIT_WRITE;
}


bool DB::waitTimeout() const {
  return status & MYSQL_WAIT_TIMEOUT;
}


int DB::getSocket() const {
  return mysql_get_socket(db);
}


double DB::getTimeout() const {
  return (double)mysql_get_timeout_value_ms(db) / 1000000.0;
}


SmartPointer<Event::Event>
DB::addEvent(cb::Event::Base &base,
             const SmartPointer<Event::EventCallback> &cb) const {
  assertPending();

  unsigned events =
    (waitRead() ? Event::Base::EVENT_READ : 0) |
    (waitWrite() ? Event::Base::EVENT_WRITE : 0) |
    (waitTimeout() ? Event::Base::EVENT_TIMEOUT : 0);

  SmartPointer<Event::Event> e = base.newEvent(getSocket(), events, cb);

  if (waitTimeout()) e->add(getTimeout());
  else e->add();

  return e;
}


string DB::escape(const string &s) const {
  SmartPointer<char>::Array to = new char[s.length() * 2 + 1];

  unsigned len =
    mysql_real_escape_string(db, to.get(), STR_DATA(s), s.length());

  return string(to.get(), len);
}


string DB::toHex(const string &s) {
  SmartPointer<char>::Array to = new char[s.length() * 2 + 1];

  unsigned len = mysql_hex_string(to.get(), STR_DATA(s), s.length());

  return string(to.get(), len);
}


void DB::threadInit() {
  if (!mysql_thread_init()) THROW("Failed to init MariaDB threads");
}


void DB::threadEnd() {
  mysql_thread_end();
}


bool DB::threadSafe() {
  return mysql_thread_safe();
}


bool DB::closeContinue(ready_t ready) {
  status = mysql_close_cont(this->db, ready_to_mysql(ready));
  if (status) return false;

  connected = false;
  return true;
}


bool DB::connectContinue(ready_t ready) {
  MYSQL *db = 0;
  status = mysql_real_connect_cont(&db, this->db, ready_to_mysql(ready));
  if (status) return false;

  if (!db) THROW("Failed to connect");
  connected = true;

  return true;
}


bool DB::useContinue(ready_t ready) {
  int ret;
  status = mysql_select_db_cont(&ret, this->db, ready_to_mysql(ready));
  if (status) return false;

  if (ret) THROW("Failed to select DB");

  return true;
}


bool DB::queryContinue(ready_t ready) {
  int ret;
  status = mysql_real_query_cont(&ret, this->db, ready_to_mysql(ready));
  if (status) return false;

  if (ret) THROW("Query failed");

  return true;
}


bool DB::storeResultContinue(ready_t ready) {
  status = mysql_store_result_cont(&res, this->db, ready_to_mysql(ready));
  if (status) return false;

  if (!res) THROW("Failed to store result");

  stored = true;

  return true;
}


bool DB::nextResultContinue(ready_t ready) {
  int ret = 0;
  status = mysql_next_result_cont(&ret, this->db, ready_to_mysql(ready));
  if (status) return false;

  if (0 < ret) THROW("Failed to get next result");

  return true;
}


bool DB::freeResultContinue(ready_t ready) {
  status = mysql_free_result_cont(res, ready_to_mysql(ready));
  if (status) return false;

  res = 0;

  return true;
}


bool DB::fetchRowContinue(ready_t ready) {
  MYSQL_ROW row = 0;
  status = mysql_fetch_row_cont(&row, res, ready_to_mysql(ready));

  return !status;
}
