#include "../include/database/database.hpp"

Database::Database(const char* filepath) {
    assert((file = ifstream(filepath, ifstream::binary)));
    file_size = filesystem::file_size(filesystem::path(filepath));
}

void Database::LoadExistingDatabase() {
    std::chrono::duration<double> list_entries_info_elapsed;
    std::chrono::duration<double> row_group_elapsed;
    std::chrono::duration<double> data_elapsed;

    //
    // load metadata about each schema and table
    //
    //
    auto start = chrono::high_resolution_clock::now();
    main_header = MainHeader(file);
    DatabaseHeader db_header_1(file, 1);
    DatabaseHeader db_header_2(file, 2);
    db_header = (db_header_1 > db_header_2) ? db_header_1 : db_header_2;
    LoadListEntries();
    auto end = chrono::high_resolution_clock::now();
    list_entries_info_elapsed = end - start;

    //
    // load metadata for row groups
    //
    //
    start = chrono::high_resolution_clock::now();
    for (auto& table : tables) {
        LoadRowGroups(table);
    }
    end = chrono::high_resolution_clock::now();
    row_group_elapsed = end - start;

    //
    // now load the actual rows
    //
    //
    start = chrono::high_resolution_clock::now();
    for (auto& table : tables) {
        LoadColumnData(table);
    }
    end = chrono::high_resolution_clock::now();
    data_elapsed = end - start;
    std::cout << "list entries metadata load time: " << list_entries_info_elapsed.count() << " sec\n"
              << "row groups load time: " << row_group_elapsed.count() << " sec\n"
              << "data load time: " << data_elapsed.count() << " sec\n";
    file.close();
}

string Database::operator[](idx_t i) {
    assert(i < data.size());
    return data[i];
}

typename vector<string>::iterator Database::begin() {
    return data.begin();
}

typename vector<string>::iterator Database::end() {
    return data.end();
}

uint64_t Database::GetRowCount() {
    return data.size();
}

void Database::LoadListEntries() {
    idx_t metablock_id = db_header.GetMetaBlockId();
    idx_t metablock_index = db_header.GetMetaBlockIndex();
    byte_t block[DEFAULT_BLOCK_SIZE];

    file.seekg(DEFAULT_HEADER_SIZE * 3 + DEFAULT_BLOCK_SIZE * metablock_id, ios::beg);
    file.read(reinterpret_cast<char *>(block), GET_READ_SIZE(file, file_size));
    byte_t* cursor = block + CHECKSUM_SIZE + METADATA_BLOCK_SIZE * metablock_index;
    Reader reader(cursor);
    
    uint8_t size = reader.Read<uint8_t>(100);
    for (auto i = 0; i < size; i++) {
        uint8_t type = reader.Read<uint8_t>(99);
        if ((CatalogType)type == CatalogType::CATALOG_SCHEMA_ENTRY) {
            Schema* schema = new Schema;
            Schema::Deserialize(reader, schema);
            schemas.push_back(schema);
        } else if ((CatalogType)type == CatalogType::CATALOG_TABLE_ENTRY) {
            Table* table = new Table;
            Table::Deserialize(reader, table);
            tables.push_back(ref(table));
        } else {
            (void)reader.Read<field_id_t>();
        }
    }
}

void Database::LoadRowGroups(Table* table) {
    byte_t block[DEFAULT_BLOCK_SIZE];
    idx_t block_id = table->GetTableStartBlockId();
    idx_t block_index = table->GetTableStartBlockIndex();
    idx_t block_offset = table->GetTableStartBlockOffset();

    file.seekg(HEADER_SIZE * 3 + block_id * DEFAULT_BLOCK_SIZE + CHECKSUM_SIZE, ios::beg);
    file.read(reinterpret_cast<char *>(block), GET_READ_SIZE(file, file_size));

    Reader reader(block);
    reader.UnalignedAdvance(METADATA_BLOCK_SIZE * block_index + block_offset);
    assert(reader.ReadEncoded<uint64_t>(100) == 1);
    (void)reader.Read<uint8_t>();
    BaseStatistics::Deserialize(100, reader);
    assert(reader.Read<field_id_t>() == OBJECT_END);  // string stats end
    assert(reader.Read<field_id_t>() == OBJECT_END);  // base stats end
    DistinctStatistics::Deserialize(101, reader);
    uint64_t n_row_groups = reader.Read<uint64_t>();
    table->SetRowGroupCount(n_row_groups);

    // now load the actual row groups
    for (uint64_t i = 0; i < n_row_groups; i++) {
        // printf("===== row group %llu =====\n", i);
        auto row_start = reader.ReadEncoded<uint64_t>(100);
        auto tuple_count = reader.ReadEncoded<uint64_t>(101);
        auto n_data_blocks = reader.ReadEncoded<uint64_t>(102);
        // printf("row_start=%llu, tuple_count=%llu, n_data_pointers=%llu\n",
        //         row_start, tuple_count, n_data_blocks);
        RowGroup* row_group = new RowGroup(row_start, tuple_count);
        row_group->ReadDataBlocks(n_data_blocks, reader);
        (void)reader.Read<uint8_t>(103);
        auto val = reader.Read<field_id_t>();
        assert(val == OBJECT_END);
        table->AddRowGroup(row_group);
        // printf("=== row group %llu done ===\n", i);
    }
}

void Database::LoadColumnData(Table* table) {
    uint64_t n_row_groups = table->GetRowGroupCount();
    for (uint64_t i = 0; i < n_row_groups; i++) {
        RowGroup* row_group = table->GetRowGroup(i);
        auto n_data_pointers = row_group->GetDataPointerCount();
        for (uint64_t j = 0; j < n_data_pointers; j++) {
            MetadataBlock data_block = row_group->GetMetaBlock(j);
            LoadColumnDataPointer(data_block);
        }
    }
    assert(data.size() == table->GetRowCount());
}

void Database::LoadColumnDataPointer(MetadataBlock& meta_block){
    idx_t block_id = meta_block.GetBlockId();
    idx_t block_index = meta_block.GetBlockIndex();
    idx_t block_offset = meta_block.GetBlockOffset();
    byte_t block[DEFAULT_BLOCK_SIZE];
    file.seekg(DEFAULT_HEADER_SIZE * 3 + DEFAULT_BLOCK_SIZE * block_id + CHECKSUM_SIZE, ios::beg);
    file.read(reinterpret_cast<char *>(block), GET_READ_SIZE(file, file_size));
    // auto offset = DEFAULT_HEADER_SIZE * 3 + DEFAULT_BLOCK_SIZE * block_id + CHECKSUM_SIZE +METADATA_BLOCK_SIZE * block_index;
    Reader reader(block);
    
    reader.UnalignedAdvance(METADATA_BLOCK_SIZE * block_index);
    idx_t next_metablock_ptr = reader.ReadMetaBlockPtr();
    reader.UnalignedAdvance(block_offset);

    auto n_data_pointers = reader.ReadEncoded<uint64_t>(100);
    for (uint64_t i = 0; i < n_data_pointers; i++) {
        auto row_start = reader.ReadEncoded<uint64_t>(100);
        auto tuple_count = reader.ReadEncoded<uint64_t>(101);
        auto data_block = StorageBlock::Deserialize(102, reader);
        auto compression = CompressionType::Deserialize(103, reader);
        (void)compression;
        BaseStatistics::Deserialize(104, reader);
        if (reader.Read<field_id_t>() != OBJECT_END) {
            assert(next_metablock_ptr != 0xffffffff);
            auto next = MetadataBlock(next_metablock_ptr, 0);
            auto next_id = next.GetBlockId();
            auto next_index = next.GetBlockIndex();
            // printf("next_id=%llu, next_index=%llu\n", next_id, next_index);
            reader.Reassign(file, file_size, next_id, next_index);
            reader.UnalignedAdvance(METADATA_BLOCK_SIZE * next_index);
            next_metablock_ptr = reader.ReadMetaBlockPtr();
            reader.UnalignedAdvance(POINTER_SIZE);
            auto val = reader.Read<field_id_t>();
            assert(val == OBJECT_END);    
        }
        assert(reader.Read<field_id_t>() == OBJECT_END);  // base stats end
        assert(reader.Read<field_id_t>() == OBJECT_END);
        // printf("row_start=%llu, tuple_count=%llu, block_id=%llu, offset=%llu\n",
        //         row_start, tuple_count, data_block.GetBlockId(), data_block.GetBlockOffset());
        LoadData(row_start, tuple_count, data_block);
    }
}

void Database::LoadData([[maybe_unused]] uint64_t row_start, uint64_t tuple_count, StorageBlock& block_pointer) {
    uint64_t block_id = block_pointer.GetBlockId();
    uint64_t block_offset = block_pointer.GetBlockOffset();
    uint64_t block_start = HEADER_SIZE * 3 + block_id * DEFAULT_BLOCK_SIZE;
    byte_t block[DEFAULT_BLOCK_SIZE];
    file.seekg(block_start, ios::beg);
    file.read(reinterpret_cast<char *>(block), GET_READ_SIZE(file, file_size));

    byte_t* cursor = block + CHECKSUM_SIZE + block_offset + sizeof(uint32_t);
    DataReader offset_reader(cursor);
    uint32_t dict_end_offset = offset_reader.Read<uint32_t>();
    uint32_t offset_array[tuple_count];
    offset_reader.Read<uint32_t>(offset_array, tuple_count);
    uint32_t total_length = offset_array[tuple_count-1];

    byte_t* strings_start = block + CHECKSUM_SIZE + block_offset + dict_end_offset - total_length;
    DataReader string_reader(strings_start);

    for (int64_t i = tuple_count-1; i >= 0; i--) {
        uint32_t length = offset_array[i];
        if (i > 0) {
            length -= offset_array[i-1];
        }
        char row[length];
        string_reader.Read<char>(row, length);
        data.push_back(string(row, length));
    }
}