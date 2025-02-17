#include "Storages/Hive/StorageCnchHive.h"
#if USE_HIVE

#include <Protos/hive_models.pb.h>
#include "CloudServices/CnchServerResource.h"
#include "DataStreams/narrowBlockInputStreams.h"
#include "Functions/FunctionFactory.h"
#include "Interpreters/ClusterProxy/SelectStreamFactory.h"
#include "Interpreters/ClusterProxy/executeQuery.h"
#include "Interpreters/InterpreterSelectQuery.h"
#include "Interpreters/PushFilterToStorage.h"
#include "Interpreters/SelectQueryOptions.h"
#include "Interpreters/evaluateConstantExpression.h"
#include "Interpreters/trySetVirtualWarehouse.h"
#include "MergeTreeCommon/CnchStorageCommon.h"
#include "Optimizer/PredicateUtils.h"
#include "Optimizer/SelectQueryInfoHelper.h"
#include "Parsers/ASTClusterByElement.h"
#include "Parsers/ASTCreateQuery.h"
#include "Parsers/ASTLiteral.h"
#include "Parsers/ASTSelectQuery.h"
#include "Parsers/ASTSetQuery.h"
#include "Parsers/queryToString.h"
#include "Processors/Sources/NullSource.h"
#include "QueryPlan/BuildQueryPipelineSettings.h"
#include "QueryPlan/Optimizations/QueryPlanOptimizationSettings.h"
#include "QueryPlan/ReadFromPreparedSource.h"
#include "ResourceManagement/CommonData.h"
#include "Storages/AlterCommands.h"
#include "Storages/DataLakes/HudiDirectoryLister.h"
#include "Storages/Hive/CnchHiveSettings.h"
#include "Storages/Hive/DirectoryLister.h"
#include "Storages/Hive/HiveFile/IHiveFile.h"
#include "Storages/Hive/HivePartition.h"
#include "Storages/Hive/HiveSchemaConverter.h"
#include "Storages/Hive/HiveWhereOptimizer.h"
#include "Storages/Hive/Metastore/HiveMetastore.h"
#include "Storages/Hive/StorageHiveSource.h"
#include "Storages/MergeTree/MergeTreeWhereOptimizer.h"
#include "Storages/MergeTree/PartitionPruner.h"
#include "Storages/StorageFactory.h"
#include "Storages/StorageInMemoryMetadata.h"
#include "Storages/DataLakes/HudiDirectoryLister.h"

#include <boost/lexical_cast.hpp>
#include <thrift/TToString.h>
#include "common/scope_guard_safe.h"

namespace DB
{
namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int UNKNOWN_FORMAT;
    extern const int SUPPORT_IS_DISABLED;
    extern const int TOO_MANY_PARTITIONS;
}

static std::optional<UInt64> get_file_hash_index(const String & hive_file_path)
{
    auto get_hash_index_from_position = [&hive_file_path] (size_t pos) -> std::optional<Int64> {
        size_t l = pos, r = pos;
        for (; r < hive_file_path.size(); ++r)
        {
            if (!std::isdigit(hive_file_path[r]))
                break;
        }
        if (l < r)
            return std::stoi(hive_file_path.substr(l, r - l));
        return {};
    };

    /// This is special format used by tea
    /// part-00000-5cf7580f-a3f6-4beb-90a6-e9f4de61c887_00003.c000
    /// 00003 : part hash index
    if (auto res = get_hash_index_from_position(hive_file_path.find_last_of('_') + 1); res)
        return res;

    /// The naming convention has the bucket number as the start of the file name
    /// Used mostly by Hive and Trino
    /// /000003_0_66add4ef-d1fc-4015-87b4-6962de044323_20240229_033029_00033_erdcf
    if (auto res = get_hash_index_from_position(hive_file_path.find_last_of('/') + 1); res)
        return res;

    return {};
}

StorageCnchHive::StorageCnchHive(
    const StorageID & table_id_,
    const String & hive_metastore_url_,
    const String & hive_db_name_,
    const String & hive_table_name_,
    std::optional<StorageInMemoryMetadata> metadata_,
    ContextPtr context_,
    IMetaClientPtr meta_client,
    std::shared_ptr<CnchHiveSettings> settings_)
    : IStorage(table_id_)
    , WithContext(context_)
    , hive_metastore_url(hive_metastore_url_)
    , hive_db_name(hive_db_name_)
    , hive_table_name(hive_table_name_)
    , hive_client(meta_client)
    , storage_settings(settings_)
{
    if (metadata_)
        initialize(*metadata_);
}

void StorageCnchHive::setHiveMetaClient(const IMetaClientPtr & client)
{
    hive_client = client;
}

void StorageCnchHive::initialize(StorageInMemoryMetadata metadata_)
{
    try
    {
        if (!hive_client)
            hive_client = HiveMetastoreClientFactory::instance().getOrCreate(hive_metastore_url, storage_settings);

        hive_table = hive_client->getTable(hive_db_name, hive_table_name);
    }
    catch (...)
    {
        hive_exception = std::current_exception();
        return;
    }

    HiveSchemaConverter converter(getContext(), hive_table);
    if (metadata_.columns.empty())
    {
        converter.convert(metadata_);
        setInMemoryMetadata(metadata_);
    }
    else
    {
        converter.check(metadata_);
        setInMemoryMetadata(metadata_);
    }
}

void StorageCnchHive::startup()
{
    /// for some reason, we do not what to throw exceptions in ctor
    if (hive_exception)
    {
        std::rethrow_exception(hive_exception);
    }
}

bool StorageCnchHive::isBucketTable() const
{
    return getInMemoryMetadata().hasClusterByKey();
}

QueryProcessingStage::Enum StorageCnchHive::getQueryProcessingStage(
    ContextPtr local_context, QueryProcessingStage::Enum, const StorageSnapshotPtr &, SelectQueryInfo &) const
{
    const auto & local_settings = local_context->getSettingsRef();

    if (local_settings.distributed_perfect_shard || local_settings.distributed_group_by_no_merge)
    {
        return QueryProcessingStage::Complete;
    }
    else if (auto worker_group = local_context->tryGetCurrentWorkerGroup())
    {
        size_t num_workers = worker_group->getShardsInfo().size();
        size_t result_size = (num_workers * local_settings.max_parallel_replicas);
        return result_size == 1 ? QueryProcessingStage::Complete : QueryProcessingStage::WithMergeableState;
    }
    else
    {
        return QueryProcessingStage::WithMergeableState;
    }
}

std::optional<String> StorageCnchHive::getVirtualWarehouseName(VirtualWarehouseType vw_type) const
{
    if (storage_settings)
    {
        if (vw_type == VirtualWarehouseType::Default)
        {
            /// deprecated
            if (storage_settings->cnch_vw_read.changed)
                return storage_settings->cnch_vw_read;

            return storage_settings->cnch_vw_default;
        }
        else if (vw_type == VirtualWarehouseType::Write)
        {
            return storage_settings->cnch_vw_write;
        }
    }
    return {};
}

void StorageCnchHive::collectResource(ContextPtr local_context, PrepareContextResult & result)
{
    auto worker_group = getWorkerGroupForTable(local_context, shared_from_this());
    auto cnch_resource = local_context->getCnchServerResource();
    auto txn_id = local_context->getCurrentTransactionID();
    StorageID cloud_storage_id = getStorageID();
    cloud_storage_id.table_name = cloud_storage_id.table_name + '_' + txn_id.toString();
    CloudTableBuilder builder;
    String cloud_table_sql
        = builder.setStorageID(cloud_storage_id).setMetadata(getInMemoryMetadataPtr()).setCloudEngine("CloudHive").build();

    LOG_INFO(log, "Create cloud table sql {}", cloud_table_sql);
    cnch_resource->addCreateQuery(local_context, shared_from_this(), cloud_table_sql, builder.cloudTableName());
    cnch_resource->addDataParts(getStorageUUID(), result.hive_files);
    result.local_table_name = builder.cloudTableName();
}

PrepareContextResult StorageCnchHive::prepareReadContext(
    const Names & column_names,
    const StorageMetadataPtr & metadata_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr & local_context,
    unsigned num_streams)
{
    metadata_snapshot->check(column_names, getVirtuals(), getStorageID());
    HiveWhereOptimizer optimizer(metadata_snapshot, query_info);

    HivePartitions partitions = selectPartitions(local_context, metadata_snapshot, query_info, optimizer);

    const auto & settings = local_context->getSettingsRef();
    if (settings.max_partitions_to_read > 0)
    {
        if (partitions.size() > static_cast<size_t>(settings.max_partitions_to_read ))
        {
            throw Exception(
                ErrorCodes::TOO_MANY_PARTITIONS,
                "Too many partitions to read. Current {}, max {}",
                partitions.size(),
                settings.max_partitions_to_read);
        }
    }
    HiveFiles hive_files;
    std::mutex mu;

    auto lister = getDirectoryLister();
    auto list_partition = [&](const HivePartitionPtr & partition) {
        HiveFiles files = lister->list(partition);
        {
            std::lock_guard lock(mu);
            hive_files.insert(hive_files.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
        }
    };

    if (num_streams <= 1 || partitions.size() == 1)
    {
        for (const auto & partition : partitions)
        {
            list_partition(partition);
        }
    }
    else
    {
        size_t num_threads = std::min(size_t(num_streams), partitions.size());

        ThreadPool pool(num_threads);
        for (const auto & partition : partitions)
        {
            pool.scheduleOrThrowOnError([&, partition, thread_group = CurrentThread::getGroup()] {
                SCOPE_EXIT_SAFE(if (thread_group) CurrentThread::detachQueryIfNotDetached(););
                if (thread_group)
                    CurrentThread::attachTo(thread_group);
                list_partition(partition);
            });
        }
        pool.wait();
    }

    size_t total_hive_files = hive_files.size();
    if (isBucketTable() && settings.use_hive_cluster_key_filter)
    {
        auto required_bucket = getSelectedBucketNumber(local_context, query_info, metadata_snapshot, optimizer);
        /// prune files with required bucket number
        auto end = std::remove_if(hive_files.begin(), hive_files.end(), [&] (auto & file) {
            if (!required_bucket)
                return false;
            auto hash_index = get_file_hash_index(file->file_path);
            return hash_index && hash_index != *required_bucket;
        });
        hive_files.erase(end, hive_files.end());
    }

    LOG_DEBUG(log, "Read from {}/{} hive files", hive_files.size(), total_hive_files);
    PrepareContextResult result{.hive_files = std::move(hive_files)};

    collectResource(local_context, result);
    return result;
}

Pipe StorageCnchHive::read(
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr local_context,
    QueryProcessingStage::Enum processed_stage,
    size_t max_block_size,
    unsigned num_streams)
{
    QueryPlan plan;
    read(plan, column_names, storage_snapshot, query_info, local_context, processed_stage, max_block_size, num_streams);
    return plan.convertToPipe(
        QueryPlanOptimizationSettings::fromContext(local_context), BuildQueryPipelineSettings::fromContext(local_context));
}

void StorageCnchHive::read(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr local_context,
    QueryProcessingStage::Enum processed_stage,
    const size_t /*max_block_size*/,
    unsigned num_streams)
{
    PrepareContextResult result = prepareReadContext(column_names, storage_snapshot->metadata, query_info, local_context, num_streams);
    Block header = InterpreterSelectQuery(query_info.query, local_context, SelectQueryOptions(processed_stage)).getSampleBlock();

    auto worker_group = getWorkerGroupForTable(local_context, shared_from_this());
    /// Return directly (with correct header) if no shard read from
    if (!worker_group || worker_group->getShardsInfo().empty() || result.hive_files.empty())
    {
        LOG_TRACE(log, " worker group empty ");
        Pipe pipe(std::make_shared<NullSource>(header));
        auto read_from_pipe = std::make_unique<ReadFromPreparedSource>(std::move(pipe));
        read_from_pipe->setStepDescription("Read from NullSource (CnchMergeTree)");
        query_plan.addStep(std::move(read_from_pipe));
        return;
    }

    const Scalars & scalars = local_context->hasQueryContext() ? local_context->getQueryContext()->getScalars() : Scalars{};
    ASTPtr select_ast = CnchStorageCommonHelper::rewriteSelectQuery(query_info.query, getDatabaseName(), result.local_table_name);

    ClusterProxy::SelectStreamFactory select_stream_factory = ClusterProxy::SelectStreamFactory(
        header,
        {},
        storage_snapshot,
        processed_stage,
        StorageID::createEmpty(), /// Don't check whether table exists in cnch-worker
        scalars,
        false,
        local_context->getExternalTables());

    ClusterProxy::executeQuery(query_plan, select_stream_factory, log, select_ast, local_context, worker_group);

    if (!query_plan.isInitialized())
        throw Exception("Pipeline is not initialized", ErrorCodes::LOGICAL_ERROR);
}

ASTPtr StorageCnchHive::applyFilter(
    ASTPtr query_filter, SelectQueryInfo & query_info, ContextPtr local_context, PlanNodeStatisticsPtr /*storage_statistics*/) const
{
    const auto & settings = local_context->getSettingsRef();
    auto * select_query = query_info.getSelectQuery();
    PushFilterToStorage push_filter_to_storage(shared_from_this(), local_context);
    ASTs conjuncts;
    /// Set partition_filter
    /// this should be done before setting query.where() to avoid partition filters being chosen as prewhere
    if (settings.external_enable_partition_filter_push_down)
    {
        auto [push_predicates, remain_predicates] = push_filter_to_storage.extractPartitionFilter(query_filter);

        query_info.appendPartitonFilters(push_predicates);
        conjuncts.swap(remain_predicates);
    }

    /// Set query.where()
    // IStorage::applyFilter(PredicateUtils::combineConjuncts(conjuncts), query_info, local_context, storage_statistics);
    select_query->setExpression(ASTSelectQuery::Expression::WHERE, PredicateUtils::combineConjuncts(conjuncts));

    /// Set prewhere()
    if (supportsPrewhere() && settings.optimize_move_to_prewhere
        && select_query->where() && !select_query->prewhere()
        && (!select_query->final() || settings.optimize_move_to_prewhere_if_final))
    {
        if (HiveMoveToPrewhereMethod::ALL == settings.hive_move_to_prewhere_method)
        {
            select_query->setExpression(ASTSelectQuery::Expression::PREWHERE, PredicateUtils::combineConjuncts(conjuncts));
        }
        else if (HiveMoveToPrewhereMethod::COLUMN_SIZE == settings.hive_move_to_prewhere_method)
        {
            /// PREWHERE optimization: transfer some condition from WHERE to PREWHERE if enabled and viable
            if (const auto & column_sizes_copy = getColumnSizes(); !column_sizes_copy.empty())
            {
                /// Extract column compressed sizes.
                std::unordered_map<std::string, UInt64> column_compressed_sizes;
                for (const auto & [name, sizes] : column_sizes_copy)
                    column_compressed_sizes[name] = sizes.data_compressed;

                auto current_info = buildSelectQueryInfoForQuery(query_info.query, local_context);
                MergeTreeWhereOptimizer{
                    current_info,
                    local_context,
                    std::move(column_compressed_sizes),
                    getInMemoryMetadataPtr(),
                    current_info.syntax_analyzer_result->requiredSourceColumns(),
                    &Poco::Logger::get("OptimizerEarlyPrewherePushdown")};
            }
        }
        else if (HiveMoveToPrewhereMethod::NEVER == settings.hive_move_to_prewhere_method)
        {
            // do nothing
        }
        else
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Unimplemented move to prewhere method");
    }

    /// remove prewhere from query plan
    if (auto prewhere = select_query->prewhere())
        PredicateUtils::subtract(conjuncts, PredicateUtils::extractConjuncts(prewhere));

    return PredicateUtils::combineConjuncts(conjuncts);
}

NamesAndTypesList StorageCnchHive::getVirtuals() const
{
    return NamesAndTypesList{{"_path", std::make_shared<DataTypeString>()}, {"_file", std::make_shared<DataTypeString>()}};
}

HivePartitions StorageCnchHive::selectPartitions(
    ContextPtr local_context,
    const StorageMetadataPtr & metadata_snapshot,
    const SelectQueryInfo & query_info,
    const HiveWhereOptimizer & optimizer)
{
    /// non-partition table
    if (!metadata_snapshot->hasPartitionKey())
    {
        auto partition = std::make_shared<HivePartition>();
        partition->load(hive_table->sd);
        return {partition};
    }

    const auto & query_settings = local_context->getSettingsRef();
    String filter = {};
    if (query_settings.use_hive_metastore_filter && optimizer.partition_key_conds)
        filter = queryToString(optimizer.partition_key_conds);

    auto apache_hive_partitions = hive_client->getPartitionsByFilter(hive_db_name, hive_table_name, filter);

    std::optional<PartitionPruner> pruner;
    if (metadata_snapshot->hasPartitionKey() && query_settings.use_hive_partition_filter)
        pruner.emplace(metadata_snapshot, query_info, local_context, false);

    /// TODO: handle non partition key case
    HivePartitions partitions;
    partitions.reserve(apache_hive_partitions.size());
    for (const auto & apache_partition : apache_hive_partitions)
    {
        auto partition = std::make_shared<HivePartition>();
        partition->load(apache_partition, metadata_snapshot->getPartitionKey());
        bool can_be_pruned = pruner && pruner->canBePruned(partition->partition_id, partition->value);
        if (!can_be_pruned)
        {
            partitions.push_back(std::move(partition));
        }
    }

    LOG_DEBUG(log, "Read from {}/{} partitions", partitions.size(), apache_hive_partitions.size());
    return partitions;
}

std::optional<UInt64> StorageCnchHive::getSelectedBucketNumber(
    [[maybe_unused]] ContextPtr local_context,
    [[maybe_unused]] SelectQueryInfo & query_info,
    const StorageMetadataPtr & metadata_snapshot,
    HiveWhereOptimizer & optimizer) const
{
    if (!isBucketTable() || !optimizer.cluster_key_conds)
        return {};

    ExpressionActionsPtr cluster_by_expression = metadata_snapshot->cluster_by_key.expression;
    const auto & required_cols = cluster_by_expression->getRequiredColumnsWithTypes();
    Block block;
    for (const auto &item : required_cols)
        block.insert(ColumnWithTypeAndName{item.type, item.name});

    MutableColumns columns = block.mutateColumns();
    ASTPtr cluster_by_conds = optimizer.cluster_key_conds;
    LOG_DEBUG(log, "Useful cluster by conditions {}. Cluster key actions {}. Input block {}",
        queryToString(cluster_by_conds), cluster_by_expression->dumpActions(), block.dumpStructure());

    std::function<void(ASTPtr)> parse_cluster_by_cond = [&] (const ASTPtr &ast) -> void {
        auto *func = ast->as<ASTFunction>();
        if (!func || !func->arguments)
            return;

        if (func->name == "equals" && func->arguments->children.size() == 2) {
            ASTPtr column = evaluateConstantExpressionOrIdentifierAsLiteral(func->arguments->children[0], local_context);
            ASTPtr field = evaluateConstantExpressionOrIdentifierAsLiteral(func->arguments->children[1], local_context);

            String column_name = column->as<ASTLiteral &>().value.safeGet<String>();
            auto & value = field->as<ASTLiteral &>().value;
            if (block.has(column_name))
            {
                size_t pos = block.getPositionByName(column_name);
                if (columns[pos]->empty())
                    columns[pos]->insert(value);
            }
        }
        else if (func->name == "and")
        {
            for (const auto & child : func->arguments->children) {
                parse_cluster_by_cond(child);
            }
        }
    };
    parse_cluster_by_cond(cluster_by_conds);

    if (std::any_of(columns.begin(), columns.end(), [] (auto &column) { return column->empty(); }))
        return {};

    block.setColumns(std::move(columns));
    cluster_by_expression->execute(block);
    String result_column_name = metadata_snapshot->cluster_by_key.expression_list_ast->children[0]->getColumnName();
    auto result_column = block.getByName(result_column_name).column;
    UInt64 required_bucket = result_column->get64(0);
    LOG_DEBUG(log, "result column: {} required bucket hash index is {}", result_column_name, required_bucket);
    return required_bucket;
}

void StorageCnchHive::checkAlterSettings(const AlterCommands & commands) const
{
    static std::set<String> supported_settings = {
        "cnch_vw_default",
        "cnch_vw_read",
        "cnch_server_vw",

        "enable_local_disk_cache"
    };

    /// Check whether the value is legal for Setting.
    /// For example, we have a setting item, `SettingBool setting_test`
    /// If you submit a Alter query: "Alter table test modify setting setting_test='abc'"
    /// Then, it will throw an Exception here, because we can't convert string 'abc' to a Bool.
    auto settings_copy = *storage_settings;

    for (auto & command : commands)
    {
        if (command.type != AlterCommand::MODIFY_SETTING)
            continue;

        for (auto & change : command.settings_changes)
        {
            if (!supported_settings.count(change.name))
                throw Exception("Setting " + change.name + " cannot be modified", ErrorCodes::SUPPORT_IS_DISABLED);

            settings_copy.set(change.name, change.value);
        }
    }
}


void StorageCnchHive::checkAlterIsPossible(const AlterCommands & commands, ContextPtr) const
{
    for (const auto & command : commands)
    {
        if (command.type != AlterCommand::Type::MODIFY_SETTING)
        {
            throw Exception(
                ErrorCodes::NOT_IMPLEMENTED, "Alter of type {} is not supported by storage {}", alterTypeToString(command.type), getName());
        }
    }
}

void StorageCnchHive::alter(const AlterCommands & params, ContextPtr local_context, TableLockHolder &)
{
    checkAlterSettings(params);

    StorageInMemoryMetadata new_metadata = getInMemoryMetadata();

    params.apply(new_metadata, local_context);
    CnchHiveSettings new_settings = storage_settings ? *storage_settings : local_context->getCnchHiveSettings();
    const auto & settings_changes = new_metadata.settings_changes->as<const ASTSetQuery &>().changes;
    new_settings.applyChanges(settings_changes);

    // HiveSchemaConverter converter(local_context, hive_table);
    // converter.check(new_metadata);

    TransactionCnchPtr txn = local_context->getCurrentTransaction();
    auto action = txn->createAction<DDLAlterAction>(shared_from_this(), local_context->getSettingsRef(), local_context->getCurrentQueryId());
    auto & alter_act = action->as<DDLAlterAction &>();
    /// replace table schema in catalog
    {
        String create_table_query = getCreateTableSql();
        ParserCreateQuery parser;
        ASTPtr ast = parseQuery(
            parser, create_table_query, local_context->getSettingsRef().max_query_size,
            local_context->getSettingsRef().max_parser_depth);

        auto & create_query = ast->as<ASTCreateQuery &>();
        if (new_metadata.settings_changes && create_query.storage)
        {
            ASTStorage & storage_ast = *create_query.storage;
            storage_ast.set(storage_ast.settings, new_metadata.settings_changes);
        }

        alter_act.setNewSchema(queryToString(ast));
    }

    txn->appendAction(action);
    txn->commitV1();
    *storage_settings = std::move(new_settings);

    setInMemoryMetadata(new_metadata);
}

std::optional<TableStatistics> StorageCnchHive::getTableStats(const Strings & columns, ContextPtr local_context)
{
    bool merge_partition_stats = local_context->getSettingsRef().merge_partition_stats;

    auto stats =  hive_client->getTableStats(hive_db_name, hive_table_name, columns, merge_partition_stats);
    if (stats)
        LOG_TRACE(log, "row_count {}", stats->row_count);
    else
        LOG_TRACE(log, "no stats");
    return stats;
}

std::vector<std::pair<String, UInt64>> StorageCnchHive::getPartitionLastModificationTime(const StorageMetadataPtr & metadata_snapshot, bool binary_format)
{
    String filter = {};
    auto apache_hive_partitions = hive_client->getPartitionsByFilter(hive_db_name, hive_table_name, filter);
    std::vector<std::pair<String, UInt64>> partition_last_modification_times;
    partition_last_modification_times.reserve(apache_hive_partitions.size());
    for (const auto & apache_partition : apache_hive_partitions)
    {

        auto partition = std::make_shared<HivePartition>();
        partition->load(apache_partition, metadata_snapshot->getPartitionKey());
        if (binary_format)
        {
            String partition_str;
            WriteBufferFromString write_buffer(partition_str);
            partition->store(write_buffer, metadata_snapshot->getPartitionKey());
            partition_last_modification_times.emplace_back(partition_str, apache_partition.lastAccessTime);
        }
        else
            partition_last_modification_times.emplace_back(partition->partition_id, apache_partition.lastAccessTime);
    }
    return partition_last_modification_times;
}

void StorageCnchHive::serializeHiveFiles(Protos::ProtoHiveFiles & proto, const HiveFiles & hive_files)
{
    /// TODO: {caoliu} hack here
    if (!hive_files.empty() && hive_files.front()->partition)
    {
        proto.set_sd_url(hive_files.front()->partition->location);
    }

    for (const auto & hive_file : hive_files)
    {
        auto * proto_file = proto.add_files();
        hive_file->serialize(*proto_file);
    }
}

std::shared_ptr<IDirectoryLister> StorageCnchHive::getDirectoryLister()
{
    auto disk = HiveUtil::getDiskFromURI(hive_table->sd.location, getContext(), *storage_settings);
    const auto & input_format = hive_table->sd.inputFormat;
    if (input_format == "org.apache.hudi.hadoop.HoodieParquetInputFormat")
    {
        return std::make_shared<HudiCowDirectoryLister>(disk);
    }
    else if (input_format == "org.apache.hadoop.hive.ql.io.parquet.MapredParquetInputFormat")
    {
        return std::make_shared<DiskDirectoryLister>(disk, IHiveFile::FileFormat::PARQUET);
    }
    else if (input_format == "org.apache.hadoop.hive.ql.io.orc.OrcInputFormat")
    {
        return std::make_shared<DiskDirectoryLister>(disk, IHiveFile::FileFormat::ORC);
    }
    else
        throw Exception(ErrorCodes::UNKNOWN_FORMAT, "Unknown hive format {}", input_format);
}

StorageID StorageCnchHive::prepareTableRead(const Names & output_columns, SelectQueryInfo & query_info, ContextPtr local_context)
{
    size_t max_streams = local_context->getSettingsRef().max_threads;
    // if (max_block_size < local_context->getSettingsRef().max_block_size)
    //     max_streams = 1; // single block single stream.
    // if (max_streams > 1 && !isRemote())
    //     max_streams *= local_context->getSettingsRef().max_streams_to_max_threads_ratio;

    auto prepare_result = prepareReadContext(output_columns, getInMemoryMetadataPtr(), query_info, local_context, max_streams);

    StorageID storage_id = getStorageID();
    storage_id.table_name = prepare_result.local_table_name;
    return storage_id;
}

void registerStorageCnchHive(StorageFactory & factory)
{
    StorageFactory::StorageFeatures features{
        .supports_settings = true,
        .supports_projections = true,
        .supports_sort_order = true,
        .supports_schema_inference = true,
    };

    factory.registerStorage(
        "CnchHive",
        [](const StorageFactory::Arguments & args) {
            ASTs & engine_args = args.engine_args;
            if (engine_args.size() != 3)
                throw Exception(
                    ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                    "Storage CnchHive require 3 arguments: hive_metastore_url, hive_db_name and hive_table_name.");

            for (auto & engine_arg : engine_args)
                engine_arg = evaluateConstantExpressionOrIdentifierAsLiteral(engine_arg, args.getLocalContext());

            String hive_metastore_url = engine_args[0]->as<ASTLiteral &>().value.safeGet<String>();
            String hive_database = engine_args[1]->as<ASTLiteral &>().value.safeGet<String>();
            String hive_table = engine_args[2]->as<ASTLiteral &>().value.safeGet<String>();

            StorageInMemoryMetadata metadata;
            std::shared_ptr<CnchHiveSettings> hive_settings = std::make_shared<CnchHiveSettings>(args.getContext()->getCnchHiveSettings());
            if (args.storage_def->settings)
            {
                hive_settings->loadFromQuery(*args.storage_def);
                metadata.settings_changes = args.storage_def->settings->ptr();
            }

            if (!args.columns.empty())
                metadata.setColumns(args.columns);

            metadata.setComment(args.comment);

            if (args.storage_def->partition_by)
            {
                ASTPtr partition_by_key = args.storage_def->partition_by->ptr();
                metadata.partition_key = KeyDescription::getKeyFromAST(partition_by_key, metadata.columns, args.getContext());
            }

            if (args.storage_def->cluster_by)
            {
                ASTPtr cluster_by_ast = args.storage_def->cluster_by->ptr();
                chassert(cluster_by_ast->children.size() == 2);
                auto bucket_num = cluster_by_ast->children[1];
                auto func_hash = makeASTFunction("javaHash", cluster_by_ast->children[0]);
                auto func_mod = makeASTFunction("hiveModulo", ASTs{func_hash, bucket_num});
                auto cluster_by_key = std::make_shared<ASTClusterByElement>(func_mod, bucket_num, -1, false, false);
                metadata.cluster_by_key = KeyDescription::getClusterByKeyFromAST(cluster_by_key, metadata.columns, args.getContext());
            }

            return StorageCnchHive::create(
                args.table_id, hive_metastore_url, hive_database, hive_table, metadata, args.getContext(), args.hive_client, hive_settings);
        },
        features);
}
}
#endif
