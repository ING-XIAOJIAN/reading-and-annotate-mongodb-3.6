// index_descriptor.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#pragma once

#include <set>
#include <string>

#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

class Collection;
class IndexCatalog;
class IndexCatalogEntry;
class IndexCatalogEntryContainer;

/**
 * A cache of information computed from the memory-mapped per-index data (OnDiskIndexData).
 * Contains accessors for the various immutable index parameters, and an accessor for the
 * mutable "head" pointer which is index-specific.
 *
 * All synchronization is the responsibility of the caller.
 */

//CollectionInfoCacheImpl::updatePlanCacheIndexEntries中完成IndexEntry和IndexDescriptor的转换

//IndexCatalogImpl::init  IndexCatalogImpl::IndexBuildBlock::init()中构造使用

//索引信息，获取所有所有信息可以参考fillOutPlannerParams
//IndexCatalogEntryImpl._descriptor为该类型

class IndexDescriptor { //索引信息通过该类表达   如ID索引  唯一索引等都是通过该类表达
public:
    enum class IndexVersion { kV0 = 0, kV1 = 1, kV2 = 2 };
    static constexpr IndexVersion kLatestIndexVersion = IndexVersion::kV2;

    static constexpr StringData k2dIndexBitsFieldName = "bits"_sd;
    static constexpr StringData k2dIndexMinFieldName = "min"_sd;
    static constexpr StringData k2dIndexMaxFieldName = "max"_sd;
    static constexpr StringData k2dsphereCoarsestIndexedLevel = "coarsestIndexedLevel"_sd;
    static constexpr StringData k2dsphereFinestIndexedLevel = "finestIndexedLevel"_sd;
    static constexpr StringData k2dsphereVersionFieldName = "2dsphereIndexVersion"_sd;
    static constexpr StringData kBackgroundFieldName = "background"_sd;
    static constexpr StringData kCollationFieldName = "collation"_sd;
    static constexpr StringData kDefaultLanguageFieldName = "default_language"_sd;
    static constexpr StringData kDropDuplicatesFieldName = "dropDups"_sd;
    static constexpr StringData kExpireAfterSecondsFieldName = "expireAfterSeconds"_sd;
    static constexpr StringData kGeoHaystackBucketSize = "bucketSize"_sd;
    static constexpr StringData kIndexNameFieldName = "name"_sd;
    static constexpr StringData kIndexVersionFieldName = "v"_sd;
    static constexpr StringData kKeyPatternFieldName = "key"_sd;
    static constexpr StringData kLanguageOverrideFieldName = "language_override"_sd;
    static constexpr StringData kNamespaceFieldName = "ns"_sd;
    static constexpr StringData kPartialFilterExprFieldName = "partialFilterExpression"_sd;
    static constexpr StringData kSparseFieldName = "sparse"_sd;
    static constexpr StringData kStorageEngineFieldName = "storageEngine"_sd;
    static constexpr StringData kTextVersionFieldName = "textIndexVersion"_sd;
    static constexpr StringData kUniqueFieldName = "unique"_sd;
    static constexpr StringData kWeightsFieldName = "weights"_sd;

    /**
     * OnDiskIndexData is a pointer to the memory mapped per-index data.
     * infoObj is a copy of the index-describing BSONObj contained in the OnDiskIndexData.
     */
    IndexDescriptor(Collection* collection, const std::string& accessMethodName, BSONObj infoObj)
        : _collection(collection),
          _accessMethodName(accessMethodName),
          _infoObj(infoObj.getOwned()),
          _numFields(infoObj.getObjectField(IndexDescriptor::kKeyPatternFieldName).nFields()),
          _keyPattern(infoObj.getObjectField(IndexDescriptor::kKeyPatternFieldName).getOwned()),
          _indexName(infoObj.getStringField(IndexDescriptor::kIndexNameFieldName)),
          _parentNS(infoObj.getStringField(IndexDescriptor::kNamespaceFieldName)),
          _isIdIndex(isIdIndexPattern(_keyPattern)),
          _sparse(infoObj[IndexDescriptor::kSparseFieldName].trueValue()),
          _unique(_isIdIndex || infoObj[kUniqueFieldName].trueValue()),
          _partial(!infoObj[kPartialFilterExprFieldName].eoo()),
          _cachedEntry(NULL) {
        _indexNamespace = makeIndexNamespace(_parentNS, _indexName);

        _version = IndexVersion::kV0;
        BSONElement e = _infoObj[IndexDescriptor::kIndexVersionFieldName];
        if (e.isNumber()) {
            _version = static_cast<IndexVersion>(e.numberInt());
        }
    }


    /**
     * Returns true if the specified index version is supported, and returns false otherwise.
     */
    static bool isIndexVersionSupported(IndexVersion indexVersion);

    /**
     * Returns a set of the currently supported index versions.
     */
    static std::set<IndexVersion> getSupportedIndexVersions();

    /**
     * Returns Status::OK() if indexes of version 'indexVersion' are allowed to be created, and
     * returns ErrorCodes::CannotCreateIndex otherwise.
     */
    static Status isIndexVersionAllowedForCreation(
        IndexVersion indexVersion,
        const ServerGlobalParams::FeatureCompatibility& featureCompatibility,
        const BSONObj& indexSpec);

    /**
     * Returns the index version to use if it isn't specified in the index specification.
     */
    static IndexVersion getDefaultIndexVersion(
        ServerGlobalParams::FeatureCompatibility::Version featureCompatibilityVersion);

    //
    // Information about the key pattern.
    //

    /**
     * Return the user-provided index key pattern.
     * Example: {geo: "2dsphere", nonGeo: 1}
     * Example: {foo: 1, bar: -1}
     */
    const BSONObj& keyPattern() const {
        return _keyPattern;
    }

    /**
     * Test only command for testing behavior resulting from an incorrect key
     * pattern.
     */
    void setKeyPatternForTest(BSONObj newKeyPattern) {
        _keyPattern = newKeyPattern;
    }

    // How many fields do we index / are in the key pattern?
    int getNumFields() const {
        return _numFields;
    }

    //
    // Information about the index's namespace / collection.
    //

    // Return the name of the index.
    const std::string& indexName() const {
        return _indexName;
    }

    // Return the name of the indexed collection.
    const std::string& parentNS() const {
        return _parentNS;
    }

    // Return the name of this index's storage area (database.table.$index)
    const std::string& indexNamespace() const {
        return _indexNamespace;
    }

    // Return the name of the access method we must use to access this index's data.
    const std::string& getAccessMethodName() const {
        return _accessMethodName;
    }

    //
    // Properties every index has
    //

    // Return what version of index this is.
    IndexVersion version() const {
        return _version;
    }

    // May each key only occur once?
    bool unique() const {
        return _unique;
    }

    // Is this index sparse?
    bool isSparse() const {
        return _sparse;
    }

    // Is this a partial index?
    bool isPartial() const {
        return _partial;
    }

    // Is this index multikey?
    bool isMultikey(OperationContext* opCtx) const;

    MultikeyPaths getMultikeyPaths(OperationContext* opCtx) const;

    bool isIdIndex() const {
        return _isIdIndex;
    }

    //
    // Properties that are Index-specific.
    //

    // Allow access to arbitrary fields in the per-index info object.  Some indices stash
    // index-specific data there.
    BSONElement getInfoElement(const std::string& name) const {
        return _infoObj[name];
    }

    //
    // "Internals" of accessing the index, used by IndexAccessMethod(s).
    //

    // Return a (rather compact) std::string representation.
    std::string toString() const {
        return _infoObj.toString();
    }

    // Return the info object.
    const BSONObj& infoObj() const {
        return _infoObj;
    }

    // Both the collection and the catalog must outlive the IndexDescriptor
    const Collection* getCollection() const {
        return _collection;
    }
    const IndexCatalog* getIndexCatalog() const;

    bool areIndexOptionsEquivalent(const IndexDescriptor* other) const;

    static bool isIdIndexPattern(const BSONObj& pattern) {
        BSONObjIterator i(pattern);
        BSONElement e = i.next();
        //_id index must have form exactly {_id : 1} or {_id : -1}.
        // Allows an index of form {_id : "hashed"} to exist but
        // do not consider it to be the primary _id index
        if (!(strcmp(e.fieldName(), "_id") == 0 && (e.numberInt() == 1 || e.numberInt() == -1)))
            return false;
        return i.next().eoo();
    }

    static std::string makeIndexNamespace(StringData ns, StringData name) {
        return ns.toString() + ".$" + name.toString();
    }

private:
    /*
        {
            "v" : 2,
            "key" : {
                    "name" : 1,
                    "male" : 1
            },
            "name" : "name_1_male_1",
            "ns" : "test.test",
            "background" : true
        },
    */
    
    // Related catalog information of the parent collection
    //所属的Collection
    Collection* _collection;

    // What access method should we use for this index?
    //btree 2d text等中的一种，参考_getAccessMethodName
    std::string _accessMethodName;

    // The BSONObj describing the index.  Accessed through the various members above.
    const BSONObj _infoObj; //配置管理信息

    // --- cached data from _infoObj
    //索引字段数量，大于一个就是联合索引
    int64_t _numFields;  // How many fields are indexed?
    //索引匹配串
    BSONObj _keyPattern;
    //索引的名字，用户可以指定
    std::string _indexName;
    std::string _parentNS;
    //索引的namespace，parentNs.$name
    std::string _indexNamespace;
    //ID索引，如果索引字段只是_id则认为是IDIndex
    bool _isIdIndex;
    //是否是稀疏索引，稀疏索引不包含NULL字段
    bool _sparse;
    //是否是唯一索引
    bool _unique;
    bool _partial;
    //数据结构版本号，0和1两个，参看btree_key_generator.h[cpp]
    IndexVersion _version;

    // only used by IndexCatalogEntryContainer to do caching for perf
    // users not allowed to touch, and not part of API
    IndexCatalogEntry* _cachedEntry;

    friend class IndexCatalog;
    friend class IndexCatalogEntryImpl;
    friend class IndexCatalogEntryContainer;
};

}  // namespace mongo

