#include "../include/database/database.hpp"

Database::Database(const char* path) {
    assert((file = ifstream(path, ifstream::binary)));
}

void Database::LoadExistingDatabase() {
    auto start = chrono::high_resolution_clock::now();
    // set headers
    main_header = MainHeader(file);
    DatabaseHeader db_header_1(file, 1);
    DatabaseHeader db_header_2(file, 2);
    this->db_header = (db_header_1 > db_header_2) ? db_header_1 : db_header_2;
    LoadListEntries();
    for (auto& table : tables) {
        LoadRowGroups(table);
    }
    auto end = chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    std::cout << "Elapsed time: " << elapsed.count() << " sec\n";
}

void Database::LoadListEntries() {
    idx_t metablock_id = db_header.GetMetaBlockId();
    idx_t metablock_index = db_header.GetMetaBlockIndex();
    // byte_t* block = new byte_t[DEFAULT_BLOCK_SIZE];
    byte_t block[DEFAULT_BLOCK_SIZE];
    file.seekg(DEFAULT_HEADER_SIZE * 3 + DEFAULT_BLOCK_SIZE * metablock_id, ios::beg);
    file.read(reinterpret_cast<char *>(block), DEFAULT_HEADER_SIZE);
    byte_t* cursor = block + CHECKSUM_SIZE + METADATA_BLOCK_SIZE * metablock_index;
    Reader reader(cursor);
    // jump over pointer to next metablock for now
    uint64_t ptr = reader.Read<idx_t>();
    assert(ptr == (idx_t)(-1));
    // read metadata about schemas and tables of this database
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
    // clean up
    // delete [] block;
}

void Database::LoadRowGroups(Table* table) {
    // byte_t* block = new byte_t[DEFAULT_BLOCK_SIZE];
    byte_t block[DEFAULT_BLOCK_SIZE];
    idx_t block_id = table->GetTableStartBlockId();
    idx_t block_index = table->GetTableStartBlockIndex();
    idx_t block_offset = table->GetTableStartBlockOffset();
    file.seekg(HEADER_SIZE * 3 + block_id * DEFAULT_BLOCK_SIZE, ios::beg);
    file.read(reinterpret_cast<char *>(block), DEFAULT_BLOCK_SIZE);
    
    // printf("offset=%llu\n", HEADER_SIZE * 3 + block_id * DEFAULT_BLOCK_SIZE + CHECKSUM_SIZE + METADATA_BLOCK_SIZE * block_index + block_offset);
    byte_t* cursor = block + CHECKSUM_SIZE + METADATA_BLOCK_SIZE * block_index + block_offset;
    Reader reader(cursor);
    assert(reader.ReadEncoded<uint64_t>(100) == 1);
    (void)reader.Read<uint8_t>();
    BaseStatistics::Deserialize(100, reader);
    DistinctStatistics::Deserialize(101, reader);
    uint64_t n_row_groups = reader.Read<uint64_t>();
    // printf("n_row_groups=%llu\n", n_row_groups);
    table->SetRowGroupCount(n_row_groups);
    // now load the actual row groups
    for (auto i = 0; i < n_row_groups; i++) {
        printf("===== row group %llu =====\n", i);
        auto row_start = reader.ReadEncoded<uint64_t>(100);
        auto tuple_count = reader.ReadEncoded<uint64_t>(101);
        auto n_data_blocks = reader.ReadEncoded<uint64_t>(102);
        printf("row_start=%llu, tuple_count=%llu, n_data_blocks=%llu\n",
                row_start, tuple_count, n_data_blocks);
        RowGroup* row_group = new RowGroup(row_start, tuple_count);
        row_group->ReadDataBlocks(n_data_blocks, reader);
        (void)reader.Read<uint8_t>(103);
        auto val = reader.Read<field_id_t>();
        assert(val == OBJECT_END);
        table->AddRowGroup(row_group);
    }
    LoadColumnData(table, reader);
    // end
    // delete [] block;
}

void Database::LoadColumnData(Table* table, Reader& reader) {
    auto n_row_groups = table->GetRowGroupCount();
    vector<string> data;
    for (auto i = 0; i < n_row_groups; i++) {
        RowGroup* row_group = table->GetRowGroup(i);
        auto n_data_pointers = row_group->GetDataPointerCount();
        for (auto j = 0; j < n_data_pointers; j++) {
            MetadataBlock data_block = row_group->GetMetaBlock(j);
            LoadColumnDataPointer(data_block, data);
        }
    }
    // printf("data.size=%llu\n", data.size());
}

void Database::LoadColumnDataPointer(MetadataBlock& meta_block, vector<string>& data){
    idx_t block_id = meta_block.GetBlockId();
    idx_t block_index = meta_block.GetBlockIndex();
    idx_t block_offset = meta_block.GetBlockOffset();
    // byte_t* block = new byte_t[DEFAULT_BLOCK_SIZE];
    byte_t block[DEFAULT_BLOCK_SIZE];
    file.seekg(DEFAULT_HEADER_SIZE * 3 + DEFAULT_BLOCK_SIZE * block_id, ios::beg);
    file.read(reinterpret_cast<char *>(block), DEFAULT_BLOCK_SIZE);
    byte_t* cursor = block + CHECKSUM_SIZE + METADATA_BLOCK_SIZE * block_index + block_offset;
    Reader reader(cursor);

    auto n_data_pointers = reader.ReadEncoded<uint64_t>(100);
    for (auto i = 0; i < n_data_pointers; i++) {
        auto row_start = reader.ReadEncoded<uint64_t>(100);
        auto tuple_count = reader.ReadEncoded<uint64_t>(101);
        auto data_block = StorageBlock::Deserialize(102, reader);
        auto compression = CompressionType::Deserialize(103, reader);
        BaseStatistics::Deserialize(104, reader);
        auto field_id = reader.Read<field_id_t>();
        if (field_id != OBJECT_END && field_id == 0xbcff) {
            reader.Advance(8);
        }
        // printf("row_start=%llu, tuple_count=%llu, block_id=%llu, block_offset=%llu\n",
        //         row_start, tuple_count, data_block.GetBlockId(), data_block.GetBlockOffset());
        LoadData(row_start, tuple_count, data_block, data);
    }

    // delete [] block;
}

void Database::LoadData(uint64_t row_start, uint64_t tuple_count, StorageBlock& block_pointer, vector<string>& rows) {
    uint64_t block_id = block_pointer.GetBlockId();
    uint64_t block_offset = block_pointer.GetBlockOffset();
    uint64_t block_start = HEADER_SIZE * 3 + block_id * DEFAULT_BLOCK_SIZE;
    // byte_t* block = new byte_t[DEFAULT_BLOCK_SIZE];
    byte_t block[DEFAULT_BLOCK_SIZE];
    file.seekg(block_start, ios::beg);
    file.read(reinterpret_cast<char *>(block), DEFAULT_BLOCK_SIZE);
    byte_t* cursor = block + CHECKSUM_SIZE + block_offset + sizeof(uint32_t);
    Reader offset_reader(cursor);
    uint32_t dict_end_offset = offset_reader.Read<uint32_t>();    
    uint32_t offset_array[tuple_count];

    offset_reader.Read<uint32_t>(offset_array, tuple_count);
    uint32_t total_length = offset_array[tuple_count-1];

    byte_t* strings_start = block + dict_end_offset - total_length;
    Reader string_reader(strings_start);

    for (int64_t i = tuple_count-1; i >= 0; i--) {
        uint32_t length = offset_array[i];
        if (i > 0) {
            length -= offset_array[i-1];
        }
        char row[length];
        string_reader.Read<char>(row, length);
        rows.push_back(string(row));
    }
    
    // delete [] block;
}