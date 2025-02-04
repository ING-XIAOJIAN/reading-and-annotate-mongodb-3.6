/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/storage_engine_metadata.h"

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <cstdio>
#include <fstream>
#include <limits>
#include <ostream>
#include <vector>

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/storage/mmap_v1/paths.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace dps = ::mongo::dotted_path_support;

namespace {
/*
-bash-4.2# /root/got/mongodb-linux-x86_64-3.6.13/bin/bsondump storage.bson 
{"storage":{"engine":"wiredTiger","options":{"directoryPerDB":true,"directoryForIndexes":true,"groupCollections":false}}}
2021-03-19T12:18:19.320+0800	1 objects found
*/

//.bson文件可以通过bsondump转为json, /home/yyz/mongodb-test/bin/bsondump --bsonFile=/data/db/storage.bson --outFile=./test.json
//转换后文件内容{"storage":{"engine":"wiredTiger","options":{"directoryPerDB":true,"directoryForIndexes":true,"groupCollections":false}}}
const std::string kMetadataBasename = "storage.bson";

/**
 * Returns true if local.ns is found in 'directory' or 'directory'/local/.
 */
bool containsMMapV1LocalNsFile(const std::string& directory) {
    boost::filesystem::path directoryPath(directory);
    return boost::filesystem::exists(directoryPath / "local.ns") ||
        boost::filesystem::exists((directoryPath / "local") / "local.ns");
}

bool fsyncFile(boost::filesystem::path path) {
    invariant(path.has_filename());
    File file;
    file.open(path.string().c_str(), /*read-only*/ false, /*direct-io*/ false);
    if (!file.is_open()) {
        return false;
    }
    file.fsync();
    return true;
}

}  // namespace

// static  下面的getStorageEngineForPath中调用该函数, initializeGlobalStorageEngine中调用该函数
std::unique_ptr<StorageEngineMetadata> StorageEngineMetadata::forPath(const std::string& dbpath) {
    std::unique_ptr<StorageEngineMetadata> metadata;

	//storage.bson是否存在，存在在读取里面的内容来构造StorageEngineMetadata类
    if (boost::filesystem::exists(boost::filesystem::path(dbpath) / kMetadataBasename)) {
        metadata.reset(new StorageEngineMetadata(dbpath));
        Status status = metadata->read();
        if (!status.isOK()) {
            error() << "Unable to read the storage engine metadata file: " << status;
            fassertFailed(28661);
        }
    }
    return metadata;
}

// static  根据storage.bson构造StorageEngineMetadata类
boost::optional<std::string> StorageEngineMetadata::getStorageEngineForPath(
    const std::string& dbpath) {
    //storage.bson
    if (auto metadata = StorageEngineMetadata::forPath(dbpath)) {
        return {metadata->getStorageEngine()};
    }

    // Fallback to checking for MMAPv1-specific files to handle upgrades from before the
    // storage.bson metadata file was introduced in 3.0.
    if (containsMMapV1LocalNsFile(dbpath)) {
        return {std::string("mmapv1")};
    }
    return {};
}

StorageEngineMetadata::StorageEngineMetadata(const std::string& dbpath) : _dbpath(dbpath) {
    reset();
}

StorageEngineMetadata::~StorageEngineMetadata() {}

void StorageEngineMetadata::reset() {
    _storageEngine.clear();
    _storageEngineOptions = BSONObj();
}

const std::string& StorageEngineMetadata::getStorageEngine() const {
    return _storageEngine;
}

const BSONObj& StorageEngineMetadata::getStorageEngineOptions() const {
    return _storageEngineOptions;
}

//mongod启动从ServiceContextMongoD::initializeGlobalStorageEngine()获取
void StorageEngineMetadata::setStorageEngine(const std::string& storageEngine) {
    _storageEngine = storageEngine;
}

//mongod启动从ServiceContextMongoD::initializeGlobalStorageEngine()获取
void StorageEngineMetadata::setStorageEngineOptions(const BSONObj& storageEngineOptions) {
    _storageEngineOptions = storageEngineOptions.getOwned();
}

Status StorageEngineMetadata::read() {
    reset();

    boost::filesystem::path metadataPath = boost::filesystem::path(_dbpath) / kMetadataBasename;

    if (!boost::filesystem::exists(metadataPath)) {
        return Status(ErrorCodes::NonExistentPath,
                      str::stream() << "Metadata file " << metadataPath.string() << " not found.");
    }

    boost::uintmax_t fileSize = boost::filesystem::file_size(metadataPath);
    if (fileSize == 0) {
        return Status(ErrorCodes::InvalidPath,
                      str::stream() << "Metadata file " << metadataPath.string()
                                    << " cannot be empty.");
    }
    if (fileSize == static_cast<boost::uintmax_t>(-1)) {
        return Status(ErrorCodes::InvalidPath,
                      str::stream() << "Unable to determine size of metadata file "
                                    << metadataPath.string());
    }

    std::vector<char> buffer(fileSize);
    try {
        std::ifstream ifs(metadataPath.c_str(), std::ios_base::in | std::ios_base::binary);
        if (!ifs) {
            return Status(ErrorCodes::FileNotOpen,
                          str::stream() << "Failed to read metadata from "
                                        << metadataPath.string());
        }

        // Read BSON from file
        ifs.read(&buffer[0], buffer.size());
        if (!ifs) {
            return Status(ErrorCodes::FileStreamFailed,
                          str::stream() << "Unable to read BSON data from "
                                        << metadataPath.string());
        }
    } catch (const std::exception& ex) {
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unexpected error reading BSON data from "
                                    << metadataPath.string()
                                    << ": "
                                    << ex.what());
    }

    BSONObj obj;
    try {
        obj = BSONObj(&buffer[0]);
    } catch (DBException& ex) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Failed to convert data in " << metadataPath.string()
                                    << " to BSON: "
                                    << ex.what());
    }

    // Validate 'storage.engine' field.
    BSONElement storageEngineElement = dps::extractElementAtPath(obj, "storage.engine");
    if (storageEngineElement.type() != mongo::String) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "The 'storage.engine' field in metadata must be a string: "
                                    << storageEngineElement.toString());
    }

    // Extract storage engine name from 'storage.engine' node.
    std::string storageEngine = storageEngineElement.String();
    if (storageEngine.empty()) {
        return Status(ErrorCodes::FailedToParse,
                      "The 'storage.engine' field in metadata cannot be empty string.");
    }
    _storageEngine = storageEngine;

    // Read storage engine options generated by storage engine factory from startup options.
    BSONElement storageEngineOptionsElement = dps::extractElementAtPath(obj, "storage.options");
    if (!storageEngineOptionsElement.eoo()) {
        if (!storageEngineOptionsElement.isABSONObj()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "The 'storage.options' field in metadata must be a string: "
                              << storageEngineOptionsElement.toString());
        }
        setStorageEngineOptions(storageEngineOptionsElement.Obj());
    }

    return Status::OK();
}

Status StorageEngineMetadata::write() const {
    if (_storageEngine.empty()) {
        return Status(ErrorCodes::BadValue,
                      "Cannot write empty storage engine name to metadata file.");
    }

    boost::filesystem::path metadataTempPath =
        boost::filesystem::path(_dbpath) / (kMetadataBasename + ".tmp");
    {
        std::ofstream ofs(metadataTempPath.c_str(), std::ios_base::out | std::ios_base::binary);
        if (!ofs) {
            return Status(
                ErrorCodes::FileNotOpen,
                str::stream() << "Failed to write metadata to " << metadataTempPath.string() << ": "
                              << errnoWithDescription());
        }

        BSONObj obj = BSON(
            "storage" << BSON("engine" << _storageEngine << "options" << _storageEngineOptions));
        ofs.write(obj.objdata(), obj.objsize());
        if (!ofs) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to write BSON data to "
                                        << metadataTempPath.string()
                                        << ": "
                                        << errnoWithDescription());
        }
    }

    // Rename temporary file to actual metadata file.
    boost::filesystem::path metadataPath = boost::filesystem::path(_dbpath) / kMetadataBasename;
    try {
        // Renaming a file (at least on POSIX) should:
        // 1) fsync the temporary file.
        // 2) perform the rename.
        // 3) fsync the to and from directory (in this case, both to and from are the same).
        if (!fsyncFile(metadataTempPath)) {
            return Status(ErrorCodes::FileRenameFailed,
                          str::stream() << "Failed to fsync new `storage.bson` file.");
        }
        boost::filesystem::rename(metadataTempPath, metadataPath);
        flushMyDirectory(metadataPath);
    } catch (const std::exception& ex) {
        return Status(ErrorCodes::FileRenameFailed,
                      str::stream() << "Unexpected error while renaming temporary metadata file "
                                    << metadataTempPath.string()
                                    << " to "
                                    << metadataPath.string()
                                    << ": "
                                    << ex.what());
    }

    return Status::OK();
}

//validateMetadata中调用，检查storage.bson中的fieldName字段的内容是否和预期的expectedValue相同
template <>
Status StorageEngineMetadata::validateStorageEngineOption<bool>(
    StringData fieldName, bool expectedValue, boost::optional<bool> defaultValue) const {
    BSONElement element = _storageEngineOptions.getField(fieldName);
    if (element.eoo()) {
        if (defaultValue && *defaultValue != expectedValue) {
            return Status(
                ErrorCodes::InvalidOptions,
                str::stream()
                    << "Requested option conflicts with the current storage engine option for "
                    << fieldName
                    << "; you requested "
                    << (expectedValue ? "true" : "false")
                    << " but the current server storage is implicitly set to "
                    << (*defaultValue ? "true" : "false")
                    << " and cannot be changed");
        }
        return Status::OK();
    }
    if (!element.isBoolean()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Expected boolean field " << fieldName << " but got "
                                    << typeName(element.type())
                                    << " instead: "
                                    << element);
    }
    if (element.boolean() == expectedValue) {
        return Status::OK();
    }
    return Status(
        ErrorCodes::InvalidOptions,
        str::stream() << "Requested option conflicts with current storage engine option for "
                      << fieldName
                      << "; you requested "
                      << (expectedValue ? "true" : "false")
                      << " but the current server storage is already set to "
                      << (element.boolean() ? "true" : "false")
                      << " and cannot be changed");
}

}  // namespace mongo
