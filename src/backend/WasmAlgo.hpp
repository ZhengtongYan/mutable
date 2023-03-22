#pragma once

#include "backend/WasmUtil.hpp"
#include <mutable/parse/AST.hpp>

// must be included after Binaryen due to conflicts, e.g. with `::wasm::Throw`
#include "backend/WasmMacro.hpp"


namespace m {

namespace wasm {

// forward declarations
template<bool IsGlobal, bool ValueInPlace> struct OpenAddressingHashTable;
struct ProbingStrategy;

/*======================================================================================================================
 * sorting
 *====================================================================================================================*/

/** Sorts the buffer \p buffer using the quicksort algorithm and a branchless binary partition algorithm.  The
 * ordering is specified by \p order where the first element is the expression to order on and the second element is
 * `true` iff ordering should be performed ascending. */
template<bool IsGlobal>
void quicksort(const Buffer<IsGlobal> &buffer, const std::vector<SortingOperator::order_type> &order);


/*======================================================================================================================
 * hashing
 *====================================================================================================================*/

/*----- bit mix functions --------------------------------------------------------------------------------------------*/

/** Mixes the bits of \p bits using the Murmur3 algorithm. */
U64 murmur3_bit_mix(U64 bits);


/*----- hash functions -----------------------------------------------------------------------------------------------*/

/** Hashes \p num_bytes bytes of \p bytes using the FNV-1a algorithm. */
U64 fnv_1a(Ptr<U8> bytes, U32 num_bytes);
/** Hashes the string \p str. */
U64 str_hash(NChar str);
/** Hashes the elements of \p values where the first element is the type of the value to hash and the second element
 * is the value itself using the Murmur3-64a algorithm. */
U64 murmur3_64a_hash(std::vector<std::pair<const Type*, SQL_t>> values);


/*----- hash tables --------------------------------------------------------------------------------------------------*/

/** Hash table to hash key-value pairs in memory. */
struct HashTable
{
    using index_t = std::size_t;
    using offset_t = int32_t;
    using size_t = uint32_t;

    // forward declaration
    template<bool IsConst> struct the_entry;

    /** Helper struct as proxy to access a single value (inclusive NULL bit) of a hash table entry. */
    template<sql_type T, bool IsConst>
    struct the_reference;

    template<sql_type T, bool IsConst>
    requires (not std::same_as<T, NChar>)
    struct the_reference<T, IsConst>
    {
        friend struct the_entry<false>;
        friend struct the_entry<true>;

        using value_t = PrimitiveExpr<typename T::type>;

        private:
        Ptr<value_t> value_;
        std::optional<Ptr<U8>> is_null_byte_;
        std::optional<U8> is_null_mask_;

        explicit the_reference(Ptr<value_t> value, std::optional<Ptr<U8>> is_null_byte, std::optional<U8> is_null_mask)
            : value_(value)
            , is_null_byte_(std::move(is_null_byte))
            , is_null_mask_(std::move(is_null_mask))
        { }
        explicit the_reference(Ptr<value_t> value, Ptr<U8> is_null_byte, U8 is_null_mask)
            : value_(value)
            , is_null_byte_(is_null_byte)
            , is_null_mask_(is_null_mask)
        { }

        public:
        explicit the_reference(Ptr<value_t> value) : value_(value) { }
        explicit the_reference(Ptr<value_t> value, Ptr<void> null_bitmap, uint8_t null_bit_offset)
            : value_(value)
            , is_null_byte_((null_bitmap + (null_bit_offset / 8)).to<uint8_t*>())
            , is_null_mask_(1U << (null_bit_offset % 8))
        { }

        ///> Discards `this`.
        void discard() {
            value_.discard();
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                is_null_byte_->discard();
                is_null_mask_->discard();
            }
        }
        ///> Returns a *deep copy* of `this`.
        the_reference clone() const {
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                return the_reference(value_.clone(), is_null_byte_->clone(), is_null_mask_->clone());
            } else {
                return the_reference(value_.clone());
            }
        }

        ///> Assigns `this` to \p _value.
        void operator=(T _value) requires (not IsConst) {
            M_insist(bool(is_null_byte_) == _value.can_be_null(), "value of non-nullable entry must not be nullable");
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                auto [value, is_null] = _value.split();
                *value_ = value;
                setbit(*is_null_byte_, is_null, *is_null_mask_);
            } else {
                *value_ = _value.insist_not_null();
            }
        }
        ///> Assigns the value of `this` to \p value.
        void set_value(value_t value) requires (not IsConst) {
            *value_ = value;
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                is_null_byte_->discard();
                is_null_mask_->discard();
            }
        }
        ///> Assigns the NULL bit of `this` to \p is_null.
        void set_null_bit(Bool is_null) requires (not IsConst) {
            value_.discard();
            M_insist(bool(is_null_byte_));
            M_insist(bool(is_null_mask_));
            setbit(*is_null_byte_, is_null, *is_null_mask_);
        }

        ///> Compares `this` with \p _value.
        Bool operator==(T _value) {
            auto [value, is_null] = _value.split();
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                auto is_null_ = (**is_null_byte_ bitand *is_null_mask_).to<bool>();
                auto equal_nulls = is_null_.clone() == is_null;
                return equal_nulls and (is_null_ or *value_ == value);
            } else {
                return not is_null and *value_ == value;
            }
        }

        ///> Loads the value of `this`.
        operator T() {
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                return T(*value_, (**is_null_byte_ bitand *is_null_mask_).to<bool>());
            } else {
                return T(*value_);
            }
        }

        ///> Selects either the reference \p tru or \p fals depending on the value of \p cond.
        friend the_reference Select(Bool cond, the_reference tru, the_reference fals) {
            M_insist(bool(tru.is_null_byte_) == bool(fals.is_null_byte_), "null byte mismatch");
            M_insist(bool(tru.is_null_mask_) == bool(fals.is_null_mask_), "null mask mismatch");
            if (tru.is_null_byte_) {
                M_insist(bool(tru.is_null_mask_));
                the_reference r(
                    /* value_=        */ Select(cond.clone(), tru.value_, fals.value_),
                    /* is_null_byte_= */ Select(cond.clone(), *tru.is_null_byte_, *fals.is_null_byte_),
                    /* is_null_mask_= */ Select(cond.clone(), *tru.is_null_mask_, *fals.is_null_mask_)
                );
                cond.discard();
                return r;
            } else {
                return the_reference(Select(cond, tru.value_, fals.value_));
            }
        }
    };

    template<bool IsConst>
    struct the_reference<NChar, IsConst>
    {
        friend struct the_entry<false>;
        friend struct the_entry<true>;

        private:
        NChar addr_;
        std::optional<Ptr<U8>> is_null_byte_;
        std::optional<U8> is_null_mask_;

        explicit the_reference(NChar addr, std::optional<Ptr<U8>> is_null_byte, std::optional<U8> is_null_mask)
            : addr_(addr)
            , is_null_byte_(std::move(is_null_byte))
            , is_null_mask_(std::move(is_null_mask))
        { }
        explicit the_reference(NChar addr, Ptr<U8> is_null_byte, U8 is_null_mask)
            : addr_(addr)
            , is_null_byte_(is_null_byte)
            , is_null_mask_(is_null_mask)
        { }

        public:
        explicit the_reference(NChar addr) : addr_(addr) { }
        explicit the_reference(NChar addr, Ptr<void> null_bitmap, uint8_t null_bit_offset)
            : addr_(addr)
            , is_null_byte_((null_bitmap + (null_bit_offset / 8)).to<uint8_t*>())
            , is_null_mask_(1U << (null_bit_offset % 8))
        { }

        ///> Discards `this`.
        void discard() {
            addr_.discard();
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                is_null_byte_->discard();
                is_null_mask_->discard();
            }
        }
        ///> Returns a *deep copy* of `this`.
        the_reference clone() const {
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                return the_reference(addr_.clone(), is_null_byte_->clone(), is_null_mask_->clone());
            } else {
                return the_reference(addr_.clone());
            }
        }

        ///> Assigns `this` to the string stored at \p addr.
        void operator=(NChar addr) requires (not IsConst) {
            M_insist(addr_.length() == addr.length() and
                     addr_.guarantees_terminating_nul() == addr.guarantees_terminating_nul(),
                     "type mismatch");
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                IF (not addr.clone().is_nullptr()) {
                    strncpy(addr_, addr.clone(), U32(addr.size_in_bytes())).discard();
                };
                setbit(*is_null_byte_, addr.is_nullptr(), *is_null_mask_);
            } else {
                Wasm_insist(not addr.clone().is_nullptr(), "value of non-nullable entry must not be nullable");
                strncpy(addr_, addr, U32(addr.size_in_bytes())).discard();
            }
        }
        ///> Assigns the value of `this` to the string stored at \p addr.
        void set_value(NChar addr) requires (not IsConst) {
            M_insist(addr_.length() == addr.length() and
                     addr_.guarantees_terminating_nul() == addr.guarantees_terminating_nul(),
                     "type mismatch");
            strncpy(addr_, addr, U32(addr.size_in_bytes())).discard();
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                is_null_byte_->discard();
                is_null_mask_->discard();
            }
        }
        ///> Assigns the NULL bit of `this` to \p is_null.
        void set_null_bit(Bool is_null) requires (not IsConst) {
            addr_.discard();
            M_insist(bool(is_null_byte_));
            M_insist(bool(is_null_mask_));
            setbit(*is_null_byte_, is_null, *is_null_mask_);
        }

        ///> Compares `this` with the string stored at \p _addr.
        Bool operator==(NChar _addr) {
            M_insist(addr_.length() == _addr.length() and
                     addr_.guarantees_terminating_nul() == _addr.guarantees_terminating_nul(),
                     "type mismatch");
            auto [addr, is_nullptr] = _addr.split();
            _Bool _equal_addrs = strncmp(addr_, NChar(addr, addr_.length(), addr_.guarantees_terminating_nul()),
                                         U32(addr_.length()), EQ);
            auto [equal_addrs_val, equal_addrs_is_null] = _equal_addrs.split();
            equal_addrs_is_null.discard(); // use potentially-null value but it is overruled if it is invalid, i.e. NULL
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                auto is_nullptr_ = (**is_null_byte_ bitand *is_null_mask_).to<bool>();
                auto equal_nulls = is_nullptr_.clone() == is_nullptr;
                return equal_nulls and (is_nullptr_ or equal_addrs_val);
            } else {
                return not is_nullptr and equal_addrs_val;
            }
        }

        ///> Loads the value of `this`.
        operator NChar() {
            if (is_null_byte_) {
                M_insist(bool(is_null_mask_));
                Bool is_null = (**is_null_byte_ bitand *is_null_mask_).to<bool>();
                return NChar(Select(is_null, Ptr<Char>::Nullptr(), addr_.val()), addr_.length(),
                             addr_.guarantees_terminating_nul());
            } else {
                return addr_;
            }
        }

        ///> Selects either the reference \p tru or \p fals depending on the value of \p cond.
        friend the_reference Select(Bool cond, the_reference tru, the_reference fals) {
            M_insist(tru.addr_.length() == fals.addr_.length() and
                     tru.addr_.guarantees_terminating_nul() == fals.addr_.guarantees_terminating_nul(),
                     "type mismatch");
            M_insist(bool(tru.is_null_byte_) == bool(fals.is_null_byte_), "null byte mismatch");
            M_insist(bool(tru.is_null_mask_) == bool(fals.is_null_mask_), "null mask mismatch");
            if (tru.is_null_byte_) {
                M_insist(bool(tru.is_null_mask_));
                the_reference r(
                    /* addr_=         */ NChar(Select(cond.clone(), tru.addr_, fals.addr_), tru.addr_.length(),
                                               tru.addr_.guarantees_terminating_nul()),
                    /* is_null_byte_= */ Select(cond.clone(), *tru.is_null_byte_, *fals.is_null_byte_),
                    /* is_null_mask_= */ Select(cond.clone(), *tru.is_null_mask_, *fals.is_null_mask_)
                );
                cond.discard();
                return r;
            } else {
                return the_reference(NChar(Select(cond, tru.addr_, fals.addr_), tru.addr_.length(),
                                               tru.addr_.guarantees_terminating_nul()));
            }
        }
    };

    template<sql_type T>
    using reference_t = the_reference<T, false>;
    template<sql_type T>
    using const_reference_t = the_reference<T, true>;

    /** Helper struct as proxy to access a hash table entry. */
    template<bool IsConst>
    struct the_entry
    {
        friend struct HashTable;

        using value_t = std::variant<
            std::monostate
#define ADD_TYPE(TYPE) , the_reference<TYPE, IsConst>
            SQL_TYPES(ADD_TYPE)
#undef ADD_TYPE
        >;

        private:
        ///> Discards the held reference of \p ref.
        static void discard(value_t &ref) {
            std::visit(overloaded {
                [](auto &r) { r.discard(); },
                [](std::monostate) { M_unreachable("invalid variant"); },
            }, ref);
        }

        private:
        ///> maps `Schema::Identifier`s to `the_reference<T, IsConst>`s that reference the stored expression
        std::unordered_map<Schema::Identifier, value_t> refs_;

        public:
        the_entry() = default;
        the_entry(const the_entry&) = delete;
        the_entry(the_entry&&) = default;

        private:
        ///> constructs an entry from references of the *opposite* const-ness
        the_entry(std::unordered_map<Schema::Identifier, typename the_entry<not IsConst>::value_t> refs) {
            refs_.reserve(refs.size());
            for (auto &p : refs) {
                std::visit(overloaded {
                    [&]<typename T>(the_reference<T, not IsConst> &r) {
                        the_reference<T, IsConst> ref(
                            r.value_, std::move(r.is_null_byte_), std::move(r.is_null_mask_)
                        );
                        refs_.emplace(std::move(p.first), std::move(ref));
                    },
                    [&](the_reference<NChar, not IsConst> &r) {
                        the_reference<NChar, IsConst> ref(
                            r.addr_, std::move(r.is_null_byte_), std::move(r.is_null_mask_)
                        );
                        refs_.emplace(std::move(p.first), std::move(ref));
                    },
                    [](std::monostate) { M_unreachable("invalid variant"); },
                }, p.second);
            }
        }

        public:
        ~the_entry() {
            for (auto &p : refs_)
                discard(p.second);
        }

        ///> Converts a non-const entry to a const entry.  The former entry is moved, i.e. must not be used afterwards.
        operator the_entry<true>() requires (not IsConst) { return the_entry<true>(std::move(refs_)); }

        ///> Returns `true` iff `this` does not contain any references.
        bool empty() const { return refs_.empty(); }

        ///> Returns `true` iff `this` contains identifier \p id.
        bool has(Schema::Identifier id) const { return refs_.contains(id); }

        ///> Adds a mapping from identifier \p id to reference \p ref.
        template<sql_type T>
        void add(Schema::Identifier id, the_reference<T, IsConst> &&ref) {
            auto res = refs_.emplace(id, std::forward<the_reference<T, IsConst>>(ref));
            M_insist(res.second, "duplicate ID");
        }

        ///> Returns the **moved** entry for identifier \p id.
        value_t extract(Schema::Identifier id) {
            auto it = refs_.find(id);
            M_insist(it != refs_.end(), "identifier not found");
            auto nh = refs_.extract(it);
            return std::move(nh.mapped());
        }
        ///> Returns the **moved** entry for identifier \p id.
        template<sql_type T>
        the_reference<T, IsConst> extract(Schema::Identifier id) {
            using type = the_reference<T, IsConst>;
            auto it = refs_.find(id);
            M_insist(it != refs_.end(), "identifier not found");
            auto nh = refs_.extract(it);
            M_insist(std::holds_alternative<type>(nh.mapped()));
            return std::move(*std::get_if<type>(&nh.mapped()));
        }

        ///> Returns the **copied** entry for identifier \p id.
        value_t get(Schema::Identifier id) const {
            auto it = refs_.find(id);
            M_insist(it != refs_.end(), "identifier not found");
            return std::visit(overloaded {
                [](auto &r) -> value_t { return r.clone(); },
                [](std::monostate) -> value_t { M_unreachable("invalid reference"); },
            }, it->second);
        }
        ///> Returns the **copied** entry for identifier \p id.
        template<sql_type T>
        the_reference<T, IsConst> get(Schema::Identifier id) const {
            using type = the_reference<T, IsConst>;
            auto it = refs_.find(id);
            M_insist(it != refs_.end(), "identifier not found");
            M_insist(std::holds_alternative<type>(it->second));
            return std::get_if<type>(&it->second)->clone();
        }
    };

    using entry_t = the_entry<false>;
    using const_entry_t = the_entry<true>;

    using callback_t = std::function<void(const_entry_t)>;

    protected:
    ///> Copies the vector of `SQL_t`s \p values.
    static std::vector<SQL_t> clone(const std::vector<SQL_t> &values) {
        std::vector<SQL_t> cpy;
        cpy.reserve(values.size());
        for (auto &value : values) {
            std::visit(overloaded {
                [&](auto &v) { cpy.push_back(v.clone()); },
                [](std::monostate) { M_unreachable("invalid variant"); },
            }, value);
        }
        return cpy;
    }

    protected:
    std::reference_wrapper<const Schema> schema_; ///< schema of hash table
    std::vector<index_t> key_indices_; ///< keys of hash table
    std::vector<index_t> value_indices_; ///< values of hash table

    public:
    HashTable() = delete;

    /** Creates a hash table with schema \p schema and keys at \p key_indices. */
    HashTable(const Schema &schema, std::vector<index_t> key_indices)
        : schema_(std::cref(schema))
        , key_indices_(std::move(key_indices))
    {
        M_insist(schema.num_entries() != 0, "hash table schema must not be empty");
        M_insist(not key_indices_.empty(), "must specify a key");

        /*----- Compute `value_indices_` as complement of `key_indices_`. -----*/
        for (index_t i = 0; i != schema.num_entries(); ++i) {
            if (contains(key_indices_, i)) continue;
            value_indices_.push_back(i);
        }
    }

    HashTable(const HashTable&) = delete;
    HashTable(HashTable&&) = default;

    virtual ~HashTable() { }

    /** Sets the high watermark, i.e. the fraction of occupied entries before growing the hash table is required, to
     * \p percentage. */
    virtual void set_high_watermark(double percentage) = 0;

    /** Clears the hash table. */
    virtual void clear() = 0;

    /** Inserts an entry into the hash table with key \p key regardless whether it already exists, i.e. duplicates
     * are allowed.  Returns a handle to the newly inserted entry which may be used to write the values for this
     * entry.  Rehashing of the hash table may be performed.  Predication is supported, i.e. an entry is always
     * inserted but can only be found later iff the predication predicate is fulfilled. */
    virtual entry_t emplace(std::vector<SQL_t> key) = 0;
    /** If no entry with key \p key already exists, inserts one into the hash table, i.e. no duplicates are inserted.
     * Returns a pair of a handle to the entry with the given key which may be used to write the values for this
     * entry and a boolean flag to indicate whether an insertion was performed.  Rehashing of the hash table may be
     * performed.  Predication is supported, i.e. an entry is always inserted but can only be found later iff the
     * predication predicate is fulfilled. */
    virtual std::pair<entry_t, Bool> try_emplace(std::vector<SQL_t> key) = 0;

    /** Tries to find an entry with key \p key in the hash table.  Returns a pair of a handle to the found entry which
     * may be used to both read and write the values of this entry (if none is found this handle points to an
     * arbitrary entry and should be ignored) and a boolean flag to indicate whether an element with the specified
     * key was found.  Predication is supported, i.e. if the predication predicate is not fulfilled, no entry will be
     * found. */
    virtual std::pair<entry_t, Bool> find(std::vector<SQL_t> key) = 0;
    /** Tries to find an entry with key \p key in the hash table.  Returns a pair of a handle to the found entry which
     * may be used to only read the values of this entry (if none is found this handle points to an arbitrary entry
     * and should be ignored) and a boolean flag to indicate whether an element with the specified key was found.
     * Predication is supported, i.e. if the predication predicate is not fulfilled, no entry will be found. */
    std::pair<const_entry_t, Bool> find(std::vector<SQL_t> key) const {
        return const_cast<HashTable*>(this)->find(std::move(key));
    }

    /** Calls \p Pipeline for each entry contained in the hash table.  At each call the argument is a handle to the
     * respective entry which may be used to read both the keys and the values of this entry. */
    virtual void for_each(callback_t Pipeline) const = 0;
    /** Calls \p Pipeline for each entry with key \p key in the hash table, where the key comparison is performed
     * predicated iff \p predicated is set.  At each call the argument is a handle to the respective entry which may be
     * used to read both the keys and the values of this entry.  Predication is supported, i.e. if the predication
     * predicate is not fulfilled, the range of entries with an equal key will be empty. */
    virtual void for_each_in_equal_range(std::vector<SQL_t> key, callback_t Pipeline, bool predicated = false) const = 0;

    public:
    /** Returns a handle to a newly created dummy entry which may be used to write the values for this entry. */
    virtual entry_t dummy_entry() = 0;

    protected:
    /** Sets the byte offsets of an entry containing values of types \p types in \p offsets_in_bytes with the starting
     * offset at \p initial_offset_in_bytes and an initial alignment requirement of \p initial_max_alignment_in_bytes.
     * To minimize padding, the values are sorted by their alignment requirement.  Returns the byte size of an entry
     * and its alignments requirement in bytes. */
    std::pair<size_t, size_t> set_byte_offsets(std::vector<offset_t> &offsets_in_bytes,
                                               const std::vector<const Type*> &types,
                                               offset_t initial_offset_in_bytes = 0,
                                               offset_t initial_max_alignment_in_bytes = 1);
};


/*----- chained hash tables ------------------------------------------------------------------------------------------*/

template<bool IsGlobal>
struct ChainedHashTable : HashTable
{
    private:
    ///> variable type dependent on whether the hash table should be globally usable
    template<typename T>
    using var_t = std::conditional_t<IsGlobal, Global<T>, Var<T>>;

    HashTable::size_t entry_size_in_bytes_; ///< entry size in bytes
    HashTable::size_t entry_max_alignment_in_bytes_; ///< alignment requirement in bytes of a single entry
    std::vector<HashTable::offset_t> entry_offsets_in_bytes_; ///< entry offsets, i.e. offsets of keys and values
    ///> offset of NULL bitmap; only specified if at least one entry is nullable
    HashTable::offset_t null_bitmap_offset_in_bytes_;
    HashTable::offset_t ptr_offset_in_bytes_; ///< offset of pointer to next entry in linked collision list

    var_t<Ptr<void>> address_; ///< base address of hash table
    var_t<U32> capacity_; ///< capacity of hash table, i.e. number of buckets / collision lists; always a power of 2
    var_t<U32> num_entries_; ///< number of occupied entries of hash table
    double high_watermark_percentage_ = 1.0; ///< fraction of occupied entries before growing the hash table is required
    var_t<U32> high_watermark_absolute_; ///< maximum number of entries before growing the hash table is required
    ///> function to perform rehashing; only possible for global hash tables since variables have to be updated
    std::optional<FunctionProxy<void(void)>> rehash_;
    std::vector<std::pair<Ptr<void>, U32>> dummy_allocations_; ///< address-size pairs of dummy entry allocations
    std::optional<var_t<Ptr<void>>> predication_dummy_; ///< dummy bucket used for predication

    public:
    /** Creates a chained hash table with schema \p schema, keys at \p key_indices, and an initial capacity
     * for \p initial_capacity buckets, i.e. collision lists.  Emits code to allocate a fresh hash table.  The hash
     * table is globally visible iff \tparam IsGlobal. */
    ChainedHashTable(const Schema &schema, std::vector<HashTable::index_t> key_indices, uint32_t initial_capacity);

    ChainedHashTable(ChainedHashTable&&) = default;

    ~ChainedHashTable();

    private:
    /** Returns the address of the first bucket, i.e. the pointer to the first collision list. */
    Ptr<void> begin() const { return address_; }
    /** Returns the address of the past-the-end bucket. */
    Ptr<void> end() const { return address_ + size_in_bytes().make_signed(); }
    /** Returns the overall size in bytes of the actual hash table, i.e. without collision list entries. */
    U32 size_in_bytes() const { return capacity_ * uint32_t(sizeof(uint32_t)); }

    public:
    /** Sets the high watermark, i.e. the fraction of occupied entries before growing the hash table is required, to
     * \p percentage. */
    void set_high_watermark(double percentage) override {
        M_insist(percentage >= 1.0, "using chained collisions the load factor should be at least 1");
        high_watermark_percentage_ = percentage;
        update_high_watermark();
    }
    private:
    /** Updates internal high watermark variables according to the currently set high watermark percentage. */
    void update_high_watermark() {
        auto high_watermark_absolute_new = high_watermark_percentage_ * capacity_.make_signed().template to<double>();
        auto high_watermark_absolute_floored = high_watermark_absolute_new.template to<int32_t>().make_unsigned();
        Wasm_insist(high_watermark_absolute_floored.clone() >= 1U,
                    "at least one entry must be allowed to insert before growing the table");
        high_watermark_absolute_ = high_watermark_absolute_floored;
    }

    /** Creates dummy entry for predication. */
    void create_predication_dummy() {
        M_insist(not predication_dummy_);
        predication_dummy_.emplace(); // since globals cannot be constructed with runtime values
        *predication_dummy_ = Module::Allocator().allocate(sizeof(uint32_t), sizeof(uint32_t));
        *predication_dummy_->template to<uint32_t*>() = 0U; // set to nullptr
    }

    public:
    void clear() override;

    entry_t emplace(std::vector<SQL_t> key) override;
    std::pair<entry_t, Bool> try_emplace(std::vector<SQL_t> key) override;

    std::pair<entry_t, Bool> find(std::vector<SQL_t> key) override;

    void for_each(callback_t Pipeline) const override;
    void for_each_in_equal_range(std::vector<SQL_t> key, callback_t Pipeline, bool predicated) const override;

    entry_t dummy_entry() override;

    private:
    /** Returns the bucket address for the key \p key by hashing it. */
    Ptr<void> hash_to_bucket(std::vector<SQL_t> key) const;

    /** Inserts an entry into the hash table with key \p key regardless whether it already exists, i.e. duplicates
     * are allowed.  Returns a handle to the newly inserted entry which may be used to write the values for this
     * entry.  No rehashing of the hash table must be performed, i.e. the hash table must have at least
     * one free entry slot. */
    entry_t emplace_without_rehashing(std::vector<SQL_t> key);

    /** Compares the key of the entry at address \p entry with \p key and returns `true` iff they are equal. */
    Bool equal_key(Ptr<void> entry, std::vector<SQL_t> key) const;

    /** Inserts the key \p key into the entry at address \p entry. */
    void insert_key(Ptr<void> entry, std::vector<SQL_t> key);

    /** Returns a handle for the entry at address \p entry which may be used to write the values of the corresponding
     * entry. */
    entry_t value_entry(Ptr<void> entry) const;
    /** Returns a handle for the entry at address \p entry which may be used to read both the keys and the values of
     * the corresponding entry. */
    const_entry_t entry(Ptr<void> entry) const;

    /** Performs rehashing, i.e. resizes the hash table to the double of its capacity (by internally creating a new
     * one) while asserting that all entries are still correctly contained in the resized hash table (by rehashing
     * and reinserting but not reallocating them into the newly created hash table). */
    void rehash();
};

using LocalChainedHashTable = ChainedHashTable<false>;
using GlobalChainedHashTable = ChainedHashTable<true>;


/*----- open addressing hash tables ----------------------------------------------------------------------------------*/

template<bool ValueInPlace>
class open_addressing_hash_table_storage;

template<>
class open_addressing_hash_table_storage<true>
{
    friend struct OpenAddressingHashTable<false, true>;
    friend struct OpenAddressingHashTable<true, true>;

    std::vector<HashTable::offset_t> entry_offsets_in_bytes_;
    HashTable::offset_t null_bitmap_offset_in_bytes_; ///< only specified if at least one entry is nullable
};

template<>
class open_addressing_hash_table_storage<false>
{
    friend struct OpenAddressingHashTable<false, false>;
    friend struct OpenAddressingHashTable<true, false>;

    std::vector<HashTable::offset_t> key_offsets_in_bytes_;
    std::vector<HashTable::offset_t> value_offsets_in_bytes_;
    HashTable::offset_t ptr_offset_in_bytes_; ///< pointer to out-of-place values
    HashTable::size_t values_size_in_bytes_;
    HashTable::size_t values_max_alignment_in_bytes_;
    HashTable::offset_t keys_null_bitmap_offset_in_bytes_; ///< only specified if at least one key entry is nullable
    HashTable::offset_t values_null_bitmap_offset_in_bytes_; ///< only specified if at least one value entry is nullable
};

struct OpenAddressingHashTableBase : HashTable
{
    friend struct ProbingStrategy;

    using ref_t = uint32_t; ///< 4 bytes for reference counting

    /** Probing strategy to handle collisions in an open addressing hash table. */
    struct ProbingStrategy
    {
        protected:
        const OpenAddressingHashTableBase &ht_; ///< open addressing hash table which uses `this` probing strategy

        public:
        ProbingStrategy(const OpenAddressingHashTableBase &ht) : ht_(ht) { }

        virtual ~ProbingStrategy() { }

        /** Returns the address of the \p skips -th (starting with index 0) slot in the bucket starting at \p bucket. */
        virtual Ptr<void> skip_slots(Ptr<void> bucket, U32 skips) const = 0;
        /** Returns the address of the slot following the \p current_step -th (starting with index 0) slot \p slot in
         * the same bucket. */
        virtual Ptr<void> advance_to_next_slot(Ptr<void> slot, U32 current_step) const = 0;
    };

    protected:
    std::unique_ptr<ProbingStrategy> probing_strategy_; ///< probing strategy to handle collisions
    HashTable::offset_t refs_offset_in_bytes_; ///< offset in bytes for reference counter
    HashTable::size_t entry_size_in_bytes_; ///< entry size in bytes
    HashTable::size_t entry_max_alignment_in_bytes_; ///< alignment requirement in bytes of a single entry
    double high_watermark_percentage_ = 1.0; ///< fraction of occupied entries before growing the hash table is required

    public:
    /** Creates an open addressing hash table with schema \p schema and keys at \p key_indices. */
    OpenAddressingHashTableBase(const Schema &schema, std::vector<index_t> key_indices)
        : HashTable(schema, std::move(key_indices))
    { }

    /** Returns the address of the first entry. */
    virtual Ptr<void> begin() const = 0;
    /** Returns the address of the past-the-end entry. */
    virtual Ptr<void> end() const = 0;
    /** Returns the capacity of the hash table. */
    virtual U32 capacity() const = 0;
    /** Returns the overall size in bytes of the hash table. */
    U32 size_in_bytes() const { return capacity() * entry_size_in_bytes_; }
    /** Returns the size in bytes of a single entry in the hash table. */
    HashTable::size_t entry_size_in_bytes() const { return entry_size_in_bytes_; }

    /** Sets the hash table's probing strategy to \tparam T. */
    template<typename T>
    void set_probing_strategy() { probing_strategy_ = std::make_unique<T>(*this); }
    protected:
    /** Returns the currently used probing strategy of the hash table. */
    const ProbingStrategy & probing_strategy() const { M_insist(bool(probing_strategy_)); return *probing_strategy_; }

    /** Returns a `Reference` to the reference counter for the entry at address \p entry. */
    Reference<ref_t> reference_count(Ptr<void> entry) const { return *(entry + refs_offset_in_bytes_).to<ref_t*>(); }

    public:
    /** Sets the high watermark, i.e. the fraction of occupied entries before growing the hash table is required, to
     * \p percentage. */
    void set_high_watermark(double percentage) override {
        M_insist(percentage > 0.0 and percentage <= 1.0, "using open addressing the load factor must be in ]0,1]");
        high_watermark_percentage_ = percentage;
        update_high_watermark();
    }
    protected:
    /** Updates internal high watermark variables according to the currently set high watermark percentage. */
    virtual void update_high_watermark() { }

    public:
    void clear() override;

    protected:
    /** Returns the bucket address for the key \p key by hashing it. */
    Ptr<void> hash_to_bucket(std::vector<SQL_t> key) const;
};

template<bool IsGlobal, bool ValueInPlace>
struct OpenAddressingHashTable : OpenAddressingHashTableBase
{
    private:
    ///> variable type dependent on whether the hash table should be globally usable
    template<typename T>
    using var_t = std::conditional_t<IsGlobal, Global<T>, Var<T>>;

    open_addressing_hash_table_storage<ValueInPlace> storage_; ///< additional fields depending on the template params
    var_t<Ptr<void>> address_; ///< base address of hash table
    var_t<U32> capacity_; ///< capacity of hash table; always a power of 2
    var_t<U32> num_entries_; ///< number of occupied entries of hash table
    var_t<U32> high_watermark_absolute_; ///< maximum number of entries before growing the hash table is required
    ///> function to perform rehashing; only possible for global hash tables since variables have to be updated
    std::optional<FunctionProxy<void(void)>> rehash_;
    std::vector<std::pair<Ptr<void>, U32>> dummy_allocations_; ///< address-size pairs of dummy entry allocations
    std::optional<var_t<Ptr<void>>> predication_dummy_; ///< dummy entry used for predication

    public:
    /** Creates an open addressing hash table with schema \p schema, keys at \p key_indices, and an initial capacity
     * for \p initial_capacity entries.  Emits code to allocate a fresh hash table.  The hash table is globally visible
     * iff \tparam IsGlobal and the values are stores in-place iff \tparam ValueInPlace. */
    OpenAddressingHashTable(const Schema &schema, std::vector<HashTable::index_t> key_indices,
                            uint32_t initial_capacity);

    OpenAddressingHashTable(OpenAddressingHashTable&&) = default;

    ~OpenAddressingHashTable();

    Ptr<void> begin() const override { return address_; }
    Ptr<void> end() const override { return address_ + (capacity_ * entry_size_in_bytes_).make_signed(); }
    U32 capacity() const override { return capacity_; }

    private:
    void update_high_watermark() override {
        auto high_watermark_absolute_new = high_watermark_percentage_ * capacity_.make_signed().template to<double>();
        auto high_watermark_absolute_floored = high_watermark_absolute_new.template to<int32_t>().make_unsigned();
        Wasm_insist(high_watermark_absolute_floored.clone() > 1U,
                    "at least one entry must be allowed to insert before growing the table");
        high_watermark_absolute_ = high_watermark_absolute_floored - 1U;
        Wasm_insist(high_watermark_absolute_ < capacity_, "at least one entry must always be unoccupied for lookups");
    }

    /** Creates dummy entry for predication. */
    void create_predication_dummy() {
        M_insist(not predication_dummy_);
        predication_dummy_.emplace(); // since globals cannot be constructed with runtime values
        *predication_dummy_ = Module::Allocator().allocate(entry_size_in_bytes_, entry_max_alignment_in_bytes_);
        dummy_allocations_.emplace_back(*predication_dummy_, entry_size_in_bytes_);
        reference_count(*predication_dummy_) = ref_t(0); // set unoccupied
    }

    public:
    entry_t emplace(std::vector<SQL_t> key) override;
    std::pair<entry_t, Bool> try_emplace(std::vector<SQL_t> key) override;

    std::pair<entry_t, Bool> find(std::vector<SQL_t> key) override;

    void for_each(callback_t Pipeline) const override;
    void for_each_in_equal_range(std::vector<SQL_t> key, callback_t Pipeline, bool predicated) const override;

    entry_t dummy_entry() override;

    private:
    /** Inserts an entry into the hash table with key \p key regardless whether it already exists, i.e. duplicates
     * are allowed.  Returns a handle to the newly inserted entry which may be used to write the values for this
     * entry.  No rehashing of the hash table must be performed, i.e. the hash table must have at least
     * one free entry slot. */
    entry_t emplace_without_rehashing(std::vector<SQL_t> key);

    /** Compares the key of the slot at address \p slot with \p key and returns `true` iff they are equal. */
    Bool equal_key(Ptr<void> slot, std::vector<SQL_t> key) const;

    /** Inserts the key \p key into the slot at address \p slot. */
    void insert_key(Ptr<void> slot, std::vector<SQL_t> key);

    /** Returns a handle which may be used to write the values of the corresponding entry.  If \tparam ValueInPlace,
     * the given pointer \p ptr is interpreted as slot pointer, otherwise, it is interpreted as pointer to the
     * out-of-place values. */
    entry_t value_entry(Ptr<void> ptr) const;
    /** Returns a handle for the slot at address \p slot which may be used to read both the keys and the values of
     * the corresponding entry. */
    const_entry_t entry(Ptr<void> slot) const;

    /** Performs rehashing, i.e. resizes the hash table to the double of its capacity (by internally creating a new
     * one) while asserting that all entries are still correctly contained in the resized hash table (by rehashing
     * and reinserting them into the newly created hash table). */
    void rehash();
};

using LocalOpenAddressingOutOfPlaceHashTable = OpenAddressingHashTable<false, false>;
using GlobalOpenAddressingOutOfPlaceHashTable = OpenAddressingHashTable<true, false>;
using LocalOpenAddressingInPlaceHashTable = OpenAddressingHashTable<false, true>;
using GlobalOpenAddressingInPlaceHashTable = OpenAddressingHashTable<true, true>;


/*----- probing strategies for open addressing hash tables -----------------------------------------------------------*/

/** Linear probing strategy, i.e. always the following slot in a bucket is accessed. */
struct LinearProbing : OpenAddressingHashTableBase::ProbingStrategy
{
    LinearProbing(const OpenAddressingHashTableBase &ht) : OpenAddressingHashTableBase::ProbingStrategy(ht) { }

    Ptr<void> skip_slots(Ptr<void> bucket, U32 skips) const override;
    Ptr<void> advance_to_next_slot(Ptr<void> slot, U32 current_step) const override;
};

/** Quadratic probing strategy, i.e. at each step i, the slot to access next in a bucket is computed by skipping i
 * slots, e.g. the thirdly accessed slot in a bucket is slot number 1+2+3=6. */
struct QuadraticProbing : OpenAddressingHashTableBase::ProbingStrategy
{
    QuadraticProbing(const OpenAddressingHashTableBase &ht) : OpenAddressingHashTableBase::ProbingStrategy(ht) { }

    Ptr<void> skip_slots(Ptr<void> bucket, U32 skips) const override;
    Ptr<void> advance_to_next_slot(Ptr<void> slot, U32 current_step) const override;
};


/*======================================================================================================================
 * explicit instantiation declarations
 *====================================================================================================================*/

extern template void quicksort(const GlobalBuffer&, const std::vector<SortingOperator::order_type>&);
extern template struct m::wasm::ChainedHashTable<false>;
extern template struct m::wasm::ChainedHashTable<true>;
extern template struct m::wasm::OpenAddressingHashTable<false, false>;
extern template struct m::wasm::OpenAddressingHashTable<false, true>;
extern template struct m::wasm::OpenAddressingHashTable<true, false>;
extern template struct m::wasm::OpenAddressingHashTable<true, true>;

}

}
