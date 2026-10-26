/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "common/json_type/ob_json_tree.h"
#include "common/json_type/ob_json_bin.h"
#include "file_scan_bridge.h"
#include "lib/time/ob_time_utility.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_server_struct.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase
{
namespace sql
{
using namespace common;
namespace
{
struct FileDescriptor
{
  explicit FileDescriptor(int fd = -1) : fd_(fd) {}
  ~FileDescriptor() { reset(); }
  void reset(int fd = -1) {
#ifndef _WIN32
    if (fd_ >= 0) { ::close(fd_); }
#endif
    fd_ = fd;
  }
  int release() { const int fd = fd_; fd_ = -1; return fd; }
  int fd_;
};

std::string string(const ObString &value)
{
  return value.empty() ? std::string() : std::string(value.ptr(), value.length());
}
ObString view(const std::string &value)
{
  return ObString(static_cast<int32_t>(value.size()), value.data());
}

// Resolve both paths, then walk the trusted root and every relative component
// with openat/O_NOFOLLOW. A symlink swap cannot redirect a checked name outside
// secure_file_priv between validation and opening the input descriptor.
int checked_open(ObSQLSessionInfo &session, const ObString &path, bool directory,
                 FileDescriptor &result, std::string &canonical)
{
  int ret = OB_SUCCESS;
#ifdef _WIN32
  ret = OB_NOT_SUPPORTED;
#else
  ObString configured;
  ObArenaAllocator policy_allocator("FileSQLPath");
  UNUSED(session);
  char root[PATH_MAX];
  char full[PATH_MAX];
  const std::string input = string(path);
  if (path.empty() || path.length() >= PATH_MAX || input.find('\0') != std::string::npos) {
    ret = OB_INVALID_ARGUMENT;
  } else if (GCTX.schema_service_ == nullptr) { ret = OB_NOT_INIT; }
  else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
      *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV, policy_allocator, configured))) {
    // The global policy may be tightened after this connection was established.
    // A cached session value would leave those existing connections unrestricted.
    ret = OB_ERR_NO_PRIVILEGE;
  } else if (configured.empty() || configured.case_compare("NULL") == 0 ||
             configured.length() >= PATH_MAX || string(configured).find('\0') != std::string::npos) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else if (::realpath(string(configured).c_str(), root) == nullptr) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else if (::realpath(input.c_str(), full) == nullptr) {
    ret = errno == ENOENT ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
  } else {
    canonical = full;
    const std::string allowed(root);
    if (canonical != allowed && (allowed != "/" &&
        (canonical.size() <= allowed.size() || canonical.compare(0, allowed.size(), allowed) != 0 ||
         canonical[allowed.size()] != '/'))) {
      ret = OB_ERR_NO_PRIVILEGE;
    }
    FileDescriptor cursor(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (OB_SUCC(ret) && cursor.fd_ < 0) { ret = OB_IO_ERROR; }
    // Walking the full canonical name from '/' also pins the root's ancestors.
    for (size_t start = 1; OB_SUCC(ret) && start < canonical.size();) {
      const size_t slash = canonical.find('/', start);
      const bool last = slash == std::string::npos;
      const std::string component = canonical.substr(start, last ? std::string::npos : slash - start);
      const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
          ((!last || directory) ? O_DIRECTORY : 0);
      const int fd = ::openat(cursor.fd_, component.c_str(), flags);
      if (fd < 0) {
        ret = errno == ENOENT ? OB_FILE_NOT_EXIST : OB_ERR_NO_PRIVILEGE;
      } else {
        cursor.reset(fd);
      }
      start = last ? canonical.size() : slash + 1;
    }
    if (OB_SUCC(ret)) {
      struct stat status;
      if (::fstat(cursor.fd_, &status) != 0) { ret = OB_IO_ERROR; }
      else if (directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)) {
        ret = OB_INVALID_ARGUMENT;
      } else { result.reset(cursor.release()); }
    }
  }
#endif
  if (ret == OB_ERR_NO_PRIVILEGE) {
    LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "File SQL path is outside secure_file_priv or changed during opening");
  }
  return ret;
}

int rust_status(file_sql::FileScan &scan)
{
  int ret = OB_SUCCESS;
  const int status = scan.status();
  if (status < 0) {
    const rust::Str error = scan.error();
    const std::string message(error.data(), error.size());
    if (status == -3) { ret = OB_NOT_SUPPORTED; LOG_USER_ERROR(OB_NOT_SUPPORTED, message.c_str()); }
    else { ret = OB_INVALID_ARGUMENT; LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.c_str()); }
  }
  return ret;
}

int make_definition(ObIAllocator &allocator, const std::string &path, const std::string &format,
                    const std::string &reader, ObString &definition)
{
  int ret = OB_SUCCESS;
  ObJsonObject object(&allocator);
  ObJsonString path_node(view(path));
  ObJsonString format_node(view(format));
  ObJsonString reader_node(view(reader));
  ObJsonBuffer buffer(&allocator);
  if (OB_FAIL(object.add("path", &path_node))) {
  } else if (OB_FAIL(object.add("format", &format_node))) {
  } else if (OB_FAIL(object.add("reader", &reader_node))) {
  } else if (OB_FAIL(object.print(buffer, false))) {
  } else if (OB_FAIL(ob_write_string(allocator, buffer.string(), definition))) {
  }
  return ret;
}

int get_definition(ObIAllocator &allocator, const ObString &definition,
                   std::string &path, std::string &format, std::string &reader)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;
  if (definition.empty() || definition.length() > 4 * 1024 * 1024) { ret = OB_INVALID_ARGUMENT; }
  else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, definition,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base))) {
  } else if (base == nullptr || base->json_type() != ObJsonNodeType::J_OBJECT || base->element_count() != 3) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *object = static_cast<ObJsonObject *>(base);
    const char *keys[] = {"path", "format", "reader"};
    std::string *outputs[] = {&path, &format, &reader};
    for (int i = 0; OB_SUCC(ret) && i < 3; ++i) {
      ObJsonNode *node = object->get_value(keys[i]);
      if (node == nullptr || node->json_type() != ObJsonNodeType::J_STRING) { ret = OB_INVALID_ARGUMENT; }
      else { *outputs[i] = string(static_cast<ObJsonString *>(node)->get_str()); }
    }
  }
  return ret;
}

int discover_files(ObSQLSessionInfo &session, const ObString &pattern, std::vector<std::string> &files)
{
  int ret = OB_SUCCESS;
#ifdef _WIN32
  ret = OB_NOT_SUPPORTED;
#else
  const std::string text = string(pattern);
  if (text.empty() || text.size() >= PATH_MAX || text.find('\0') != std::string::npos) { return OB_INVALID_ARGUMENT; }
  for (size_t start = 0; start < text.size();) {
    const size_t end = text.find('/', start);
    if (text.substr(start, end == std::string::npos ? end : end - start) == "..") { return OB_ERR_NO_PRIVILEGE; }
    start = end == std::string::npos ? text.size() : end + 1;
  }
  const size_t wildcard = text.find_first_of("*?[");
  const size_t prefix_end = text.rfind('/', wildcard == std::string::npos ? text.size() : wildcard);
  const std::string directory = prefix_end == std::string::npos ? "." :
      (prefix_end == 0 ? "/" : text.substr(0, prefix_end));
  FileDescriptor checked;
  std::string canonical;
  if (OB_FAIL(checked_open(session, view(directory), true, checked, canonical))) { return ret; }
  glob_t paths = {};
  const int status = ::glob(text.c_str(), GLOB_NOSORT, nullptr, &paths);
  if (status != 0 && status != GLOB_NOMATCH) { ret = OB_IO_ERROR; }
  else if (paths.gl_pathc > 10000) { ret = OB_SIZE_OVERFLOW; }
  for (size_t i = 0; OB_SUCC(ret) && i < paths.gl_pathc; ++i) {
    struct stat st;
    if (::stat(paths.gl_pathv[i], &st) != 0) { ret = OB_IO_ERROR; }
    else if (S_ISDIR(st.st_mode)) { /* glob() exposes files only */ }
    else {
      FileDescriptor fd;
      std::string full;
      if (OB_FAIL(checked_open(session, ObString(paths.gl_pathv[i]), false, fd, full))) {
      } else { files.push_back(full); }
    }
  }
  ::globfree(&paths);
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
#endif
  return ret;
}
}

struct FileSqlReader::Impl
{
  std::optional<rust::Box<file_sql::FileScan>> scan_;
  std::vector<file_sql::ColumnKind> kinds_;
  std::vector<uint32_t> projection_;
  std::vector<std::string> files_;
  size_t position_ = 0;
  bool glob_ = false;
};
FileSqlReader::FileSqlReader() : impl_(nullptr) {}
FileSqlReader::~FileSqlReader() { close(); }
void FileSqlReader::close() { delete impl_; impl_ = nullptr; }
const char *FileSqlReader::type_name(uint8_t kind)
{
  static const char *names[] = {"NULL", "BIGINT", "DOUBLE", "BOOLEAN", "VARCHAR", "DATE", "DATETIME"};
  return kind < ARRAYSIZEOF(names) ? names[kind] : "VARCHAR";
}
int FileSqlReader::check_enabled(ObSQLSessionInfo &session)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled; SET SESSION enable_file_sql = ON");
  } else if (GCTX.schema_service_ == nullptr) { ret = OB_NOT_INIT; }
  else {
    // Match SQL's FILE privilege, including active roles, before even reading
    // a header or Footer during resolution. Enabling the feature grants no file access.
    ObArenaAllocator allocator("FileSQLPriv");
    share::schema::ObSchemaGetterGuard guard;
    share::schema::ObSessionPrivInfo info;
    share::schema::ObStmtNeedPrivs required(allocator);
    share::schema::ObNeedPriv file("", "", share::schema::OB_PRIV_USER_LEVEL, OB_PRIV_FILE, false);
    if (OB_FAIL(session.get_session_priv_info(info))) {
    } else if (OB_FAIL(required.need_privs_.init(1))) {
    } else if (OB_FAIL(required.need_privs_.push_back(file))) {
    } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    } else { ret = guard.check_priv(info, session.get_enable_role_array(), required); }
  }
  return ret;
}
int FileSqlReader::infer(ObSQLSessionInfo &session, ObExecContext *context, ObIAllocator &allocator,
                         const ObString &path, const ObString &format, const ObString &options,
                         ObString &definition, ObIArray<FileSqlColumn> &columns)
{
  int ret = OB_SUCCESS;
  FileDescriptor fd;
  std::string canonical;
  const std::string kind = string(format);
  if (OB_FAIL(check_enabled(session))) { return ret; }
  if (kind == "glob") {
    std::vector<std::string> files;
    if (OB_FAIL(discover_files(session, path, files))) { return ret; }
    FileSqlColumn column;
    column.name_ = ObString("file"); column.meta_.set_varchar();
    column.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    column.meta_.set_collation_level(CS_LEVEL_IMPLICIT);
    column.accuracy_ = ObAccuracy::DDL_DEFAULT_ACCURACY[ObVarcharType];
    column.accuracy_.set_length(PATH_MAX); column.nullable_ = false; column.kind_ = 4;
    if (OB_FAIL(columns.push_back(column))) {
    } else if (OB_FAIL(make_definition(allocator, string(path), kind, "", definition))) {
    }
    return ret;
  }
  if (OB_FAIL(checked_open(session, path, false, fd, canonical))) { return ret; }
  auto scan = file_sql::scan_open(fd.fd_, rust::Str(kind), rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(options.ptr()), options.length()));
  if (OB_FAIL(rust_status(*scan))) { return ret; }
  while (OB_SUCC(ret)) {
    const int status = scan->infer(1024);
    if (status < 0) { ret = rust_status(*scan); }
    else if (status == 1) { break; }
    if (OB_SUCC(ret) && context != nullptr) { ret = context->check_status(); }
  }
  const auto schema = scan->columns();
  for (size_t i = 0; OB_SUCC(ret) && i < schema.size(); ++i) {
    FileSqlColumn column;
    const auto &source = schema[i];
    column.kind_ = static_cast<uint8_t>(source.kind);
    column.nullable_ = source.nullable;
    const ObObjType types[] = {ObNullType, ObIntType, ObDoubleType, ObTinyIntType, ObVarcharType, ObDateType, ObDateTimeType};
    if (column.kind_ >= ARRAYSIZEOF(types)) { ret = OB_ERR_UNEXPECTED; }
    else {
      const ObObjType type = types[column.kind_];
      column.meta_.set_type(type);
      if (type == ObVarcharType) {
        column.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        column.meta_.set_collation_level(CS_LEVEL_IMPLICIT);
      }
      column.accuracy_ = ObAccuracy::DDL_DEFAULT_ACCURACY[type];
      if (type == ObVarcharType) { column.accuracy_.set_length(OB_MAX_VARCHAR_LENGTH); }
      if (type == ObDateTimeType) { column.accuracy_.set_scale(source.scale); }
      const ObString name(static_cast<int32_t>(source.name.size()), source.name.data());
      if (OB_FAIL(ob_write_string(allocator, name, column.name_))) {
      } else if (OB_FAIL(columns.push_back(column))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    const rust::String reader = scan->definition();
    ret = make_definition(allocator, canonical, kind, std::string(reader.data(), reader.size()), definition);
  }
  return ret;
}
int FileSqlReader::open(ObSQLSessionInfo &session, const ObString &definition,
                        const ObIArray<uint64_t> &column_ids)
{
  int ret = OB_SUCCESS;
  close();
  ObArenaAllocator allocator("FileSQLMeta");
  std::string path, format, reader;
  if (OB_FAIL(check_enabled(session))) {
  } else if (OB_FAIL(get_definition(allocator, definition, path, format, reader))) {
  } else if ((impl_ = new(std::nothrow) Impl()) == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  else {
    impl_->glob_ = format == "glob";
    if (impl_->glob_) {
      impl_->kinds_.push_back(file_sql::ColumnKind::Text);
      ret = discover_files(session, view(path), impl_->files_);
    } else {
      FileDescriptor fd;
      std::string canonical;
      if (OB_FAIL(checked_open(session, view(path), false, fd, canonical))) {
      } else {
        impl_->scan_.emplace(file_sql::scan_restore(fd.fd_, rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(reader.data()), reader.size())));
        auto &scan = **impl_->scan_;
        if (OB_FAIL(rust_status(scan))) {
        } else {
          const auto columns = scan.columns();
          for (const auto &column : columns) { impl_->kinds_.push_back(column.kind); }
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
      const uint64_t id = column_ids.at(i);
      if (id < OB_APP_MIN_COLUMN_ID || id - OB_APP_MIN_COLUMN_ID >= impl_->kinds_.size()) { ret = OB_INVALID_ARGUMENT; }
      else { impl_->projection_.push_back(static_cast<uint32_t>(id - OB_APP_MIN_COLUMN_ID)); }
    }
    if (OB_SUCC(ret) && !impl_->glob_) {
      (**impl_->scan_).project(rust::Slice<const uint32_t>(impl_->projection_.data(), impl_->projection_.size()));
      ret = rust_status(**impl_->scan_);
    }
  }
  if (OB_FAIL(ret)) { close(); }
  return ret;
}
int FileSqlReader::next(ObIAllocator &allocator, ObIArray<ObObj> &values)
{
  int ret = OB_SUCCESS;
  values.reuse();
  if (impl_ == nullptr) { return OB_NOT_INIT; }
  if (impl_->glob_) {
    if (impl_->position_ >= impl_->files_.size()) { return OB_ITER_END; }
    if (!impl_->projection_.empty()) {
      ObString copy;
      ObObj value;
      if (OB_FAIL(ob_write_string(allocator, view(impl_->files_[impl_->position_]), copy))) {
      } else { value.set_varchar(copy); value.set_collation_type(CS_TYPE_UTF8MB4_BIN); ret = values.push_back(value); }
    }
    ++impl_->position_;
    return ret;
  }
  auto &scan = **impl_->scan_;
  const auto row = scan.next_row();
  if (OB_FAIL(rust_status(scan))) { return ret; }
  if (scan.status() == 1) { return OB_ITER_END; }
  if (row.size() != impl_->projection_.size()) { return OB_ERR_UNEXPECTED; }
  for (size_t i = 0; OB_SUCC(ret) && i < row.size(); ++i) {
    ObObj value;
    const auto &cell = row[i];
    const auto kind = impl_->kinds_[impl_->projection_[i]];
    if (cell.is_null) { value.set_null(); }
    else if (kind == file_sql::ColumnKind::Integer) { value.set_int(cell.integer); }
    else if (kind == file_sql::ColumnKind::Boolean) { value.set_tinyint(cell.integer != 0); }
    else if (kind == file_sql::ColumnKind::Double) { value.set_double(cell.number); }
    else if (kind == file_sql::ColumnKind::Date) { value.set_date(static_cast<int32_t>(cell.integer)); }
    else if (kind == file_sql::ColumnKind::DateTime) { value.set_datetime(cell.integer); }
    else {
      ObString copy;
      const ObString text(static_cast<int32_t>(cell.text.size()), cell.text.data());
      if (OB_FAIL(ob_write_string(allocator, text, copy))) {
      } else { value.set_varchar(copy); value.set_collation_type(CS_TYPE_UTF8MB4_BIN); }
    }
    if (OB_SUCC(ret)) { ret = values.push_back(value); }
  }
  return ret;
}
int FileSqlReader::rescan()
{
  int ret = OB_SUCCESS;
  if (impl_ == nullptr) { ret = OB_NOT_INIT; }
  else if (impl_->glob_) { impl_->position_ = 0; }
  else { (**impl_->scan_).rewind(); ret = rust_status(**impl_->scan_); }
  return ret;
}
}
}

namespace oceanbase
{
namespace sql
{
struct FileSqlWriter::Impl
{
  std::optional<rust::Box<file_sql::FileWriter>> writer_;
  FileDescriptor parent_;
  std::string temporary_;
  std::string target_;
  ~Impl() {
    writer_.reset();
#ifndef _WIN32
    if (!temporary_.empty() && parent_.fd_ >= 0) { ::unlinkat(parent_.fd_, temporary_.c_str(), 0); }
#endif
  }
  int status() {
    if (!writer_.has_value()) { return OB_NOT_INIT; }
    const int status = (**writer_).writer_status();
    if (status >= 0) { return OB_SUCCESS; }
    const rust::Str error = (**writer_).writer_error();
    const std::string message(error.data(), error.size());
    const int ret = status == -3 ? OB_NOT_SUPPORTED : status == -2 ? OB_IO_ERROR : OB_INVALID_ARGUMENT;
    if (ret == OB_NOT_SUPPORTED) { LOG_USER_ERROR(OB_NOT_SUPPORTED, message.c_str()); }
    else if (ret == OB_INVALID_ARGUMENT) { LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.c_str()); }
    return ret;
  }
};
FileSqlWriter::FileSqlWriter() : impl_(nullptr) {}
FileSqlWriter::~FileSqlWriter() { close(); }
void FileSqlWriter::close() { delete impl_; impl_ = nullptr; }
int FileSqlWriter::open(ObSQLSessionInfo &session, const ObString &path, const ObString &definition)
{
  int ret = OB_SUCCESS;
  close();
#ifdef _WIN32
  ret = OB_NOT_SUPPORTED;
#else
  const std::string target = string(path);
  const size_t slash = target.rfind('/');
  const std::string parent = slash == std::string::npos ? "." : slash == 0 ? "/" : target.substr(0, slash);
  const std::string name = slash == std::string::npos ? target : target.substr(slash + 1);
  if (target.empty() || target.size() >= PATH_MAX || target.find('\0') != std::string::npos ||
      name.empty() || name == "." || name == "..") { return OB_INVALID_ARGUMENT; }
  if (OB_FAIL(FileSqlReader::check_enabled(session))) { return ret; }
  if ((impl_ = new(std::nothrow) Impl()) == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  std::string canonical;
  ObArenaAllocator allocator("FileSQLCopy");
  ObIJsonBase *base = nullptr;
  ObJsonBuffer options(&allocator), headers(&allocator);
  if (OB_FAIL(checked_open(session, view(parent), true, impl_->parent_, canonical))) {
  } else if (definition.empty() || definition.length() > 4 * 1024 * 1024) { ret = OB_INVALID_ARGUMENT; }
  else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, definition,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base))) {
  } else if (base == nullptr || base->json_type() != ObJsonNodeType::J_OBJECT || base->element_count() != 2) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *object = static_cast<ObJsonObject *>(base);
    ObJsonNode *opt = object->get_value("options");
    ObJsonNode *names = object->get_value("headers");
    if (opt == nullptr || names == nullptr || opt->json_type() != ObJsonNodeType::J_OBJECT ||
        names->json_type() != ObJsonNodeType::J_ARRAY) { ret = OB_INVALID_ARGUMENT; }
    else if (OB_FAIL(opt->print(options, false))) {
    } else if (OB_FAIL(names->print(headers, false))) {
    }
  }
  FileDescriptor output;
  if (OB_SUCC(ret)) {
    struct stat status;
    if (::fstatat(impl_->parent_.fd_, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) { ret = OB_FILE_ALREADY_EXIST; }
    else if (errno != ENOENT) { ret = OB_IO_ERROR; }
    else {
      impl_->target_ = name;
      for (int attempt = 0; OB_SUCC(ret) && output.fd_ < 0 && attempt < 64; ++attempt) {
        const std::string temporary = ".seekdb-copy-" + std::to_string(::getpid()) + "-" +
            std::to_string(ObTimeUtility::current_time()) + "-" + std::to_string(attempt);
        const int fd = ::openat(impl_->parent_.fd_, temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) { output.reset(fd); impl_->temporary_ = temporary; }
        else if (errno != EEXIST) { ret = OB_IO_ERROR; }
      }
      if (OB_SUCC(ret) && output.fd_ < 0) { ret = OB_IO_ERROR; }
    }
  }
  if (OB_SUCC(ret)) {
    impl_->writer_.emplace(file_sql::writer_open(output.fd_,
        rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(options.ptr()), options.length()),
        rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(headers.ptr()), headers.length())));
    ret = impl_->status();
  }
  if (OB_FAIL(ret)) { close(); }
#endif
  return ret;
}
int FileSqlWriter::field(const ObString &text, bool is_null)
{
  if (impl_ == nullptr || !impl_->writer_) { return OB_NOT_INIT; }
  (**impl_->writer_).write_field(rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(text.ptr()), text.length()), is_null);
  return impl_->status();
}
int FileSqlWriter::end_row()
{
  if (impl_ == nullptr || !impl_->writer_) { return OB_NOT_INIT; }
  (**impl_->writer_).write_row();
  return impl_->status();
}
int FileSqlWriter::finish()
{
  int ret = OB_SUCCESS;
  if (impl_ == nullptr || !impl_->writer_) { return OB_NOT_INIT; }
  (**impl_->writer_).finish();
  if (OB_FAIL(impl_->status())) { return ret; }
#ifndef _WIN32
  // linkat is atomic and refuses an existing destination, including a symlink.
  // All data and the CSV header are durable before making this name visible.
  if (!impl_->temporary_.empty()) {
    if (::linkat(impl_->parent_.fd_, impl_->temporary_.c_str(), impl_->parent_.fd_, impl_->target_.c_str(), 0) != 0) {
      ret = errno == EEXIST ? OB_FILE_ALREADY_EXIST : OB_IO_ERROR;
    } else {
      // Publication is the commit point. Later cleanup cannot turn a complete
      // exported file into a failed query; close() retries temporary-name cleanup.
      if (::unlinkat(impl_->parent_.fd_, impl_->temporary_.c_str(), 0) == 0) { impl_->temporary_.clear(); }
      else { LOG_WARN("COPY temporary-name cleanup failed", K(errno)); }
      if (::fsync(impl_->parent_.fd_) != 0) { LOG_WARN("COPY directory sync failed", K(errno)); }
    }
  }
#else
  ret = OB_NOT_SUPPORTED;
#endif
  return ret;
}
}
}
