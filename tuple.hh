/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include "types.hh"
#include <iostream>
#include <algorithm>
#include <vector>
#include <boost/range/iterator_range.hpp>
#include "util/serialization.hh"
#include "unimplemented.hh"

// TODO: Add AllowsMissing parameter which will allow to optimize serialized format.
// Currently we default to AllowsMissing = true.
template<bool AllowPrefixes = false>
class tuple_type final {
private:
    const std::vector<shared_ptr<abstract_type>> _types;
    const bool _byte_order_equal;
public:
    using prefix_type = tuple_type<true>;
    using value_type = std::vector<bytes_opt>;

    tuple_type(std::vector<shared_ptr<abstract_type>> types)
        : _types(std::move(types))
        , _byte_order_equal(std::all_of(_types.begin(), _types.end(), [] (auto t) {
                return t->is_byte_order_equal();
            }))
    { }

    tuple_type(tuple_type&&) = default;

    auto const& types() {
        return _types;
    }

    prefix_type as_prefix() {
        return prefix_type(_types);
    }

    /*
     * Format:
     *   <len(value1)><value1><len(value2)><value2>...
     *
     *   if value is missing then len(value) < 0
     */
    void serialize_value(const value_type& values, bytes::iterator& out) {
        if (AllowPrefixes) {
            assert(values.size() <= _types.size());
        } else {
            assert(values.size() == _types.size());
        }

        for (auto&& val : values) {
            if (!val) {
                write<uint32_t>(out, uint32_t(-1));
            } else {
                assert(val->size() <= std::numeric_limits<int32_t>::max());
                write<uint32_t>(out, uint32_t(val->size()));
                out = std::copy(val->begin(), val->end(), out);
            }
        }
    }
    bytes serialize_value(const value_type& values) {
        return ::serialize_value(*this, values);
    }
    bytes serialize_value_deep(const std::vector<boost::any>& values) {
        // TODO: Optimize
        std::vector<bytes_opt> partial;
        auto i = _types.begin();
        for (auto&& component : values) {
            assert(i != _types.end());
            partial.push_back({(*i++)->decompose(component)});
        }
        return serialize_value(partial);
    }
    bytes decompose_value(const value_type& values) {
        return ::serialize_value(*this, values);
    }
    class iterator : public std::iterator<std::forward_iterator_tag, std::experimental::optional<bytes_view>> {
    private:
        ssize_t _types_left;
        bytes_view _v;
        value_type _current;
    private:
        void read_current() {
            if (_types_left == 0) {
                if (!_v.empty()) {
                    throw marshal_exception();
                }
                _v = bytes_view(nullptr, 0);
                return;
            }
            if (_v.empty()) {
                if (AllowPrefixes) {
                    _v = bytes_view(nullptr, 0);
                    return;
                } else {
                    throw marshal_exception();
                }
            }
            auto len = read_simple<int32_t>(_v);
            if (len < 0) {
                _current = std::experimental::optional<bytes_view>();
            } else {
                auto u_len = static_cast<uint32_t>(len);
                if (_v.size() < u_len) {
                    throw marshal_exception();
                }
                _current = std::experimental::make_optional(bytes_view(_v.begin(), u_len));
                _v.remove_prefix(u_len);
            }
        }
    public:
        struct end_iterator_tag {};
        iterator(const tuple_type& t, const bytes_view& v) : _types_left(t._types.size()), _v(v) {
            read_current();
        }
        iterator(end_iterator_tag, const bytes_view& v) : _v(nullptr, 0) {}
        iterator& operator++() {
            --_types_left;
            read_current();
            return *this;
        }
        const value_type& operator*() const { return _current; }
        bool operator!=(const iterator& i) const { return _v.begin() != i._v.begin(); }
        bool operator==(const iterator& i) const { return _v.begin() == i._v.begin(); }
    };
    iterator begin(const bytes_view& v) const {
        return iterator(*this, v);
    }
    iterator end(const bytes_view& v) const {
        return iterator(typename iterator::end_iterator_tag(), v);
    }
    auto iter_items(const bytes_view& v) {
        return boost::iterator_range<iterator>(begin(v), end(v));
    }
    value_type deserialize_value(bytes_view v) {
        std::vector<bytes_opt> result;
        result.reserve(_types.size());
        std::transform(begin(v), end(v), std::back_inserter(result), [] (auto&& value_opt) {
            if (!value_opt) {
                return bytes_opt();
            }
            return bytes_opt(bytes(value_opt->begin(), value_opt->end()));
        });
        return result;
    }
    object_opt deserialize(bytes_view v) {
        return {boost::any(deserialize_value(v))};
    }
    void serialize(const boost::any& obj, bytes::iterator& out) {
        serialize_value(boost::any_cast<const value_type&>(obj), out);
    }
    size_t serialized_size(const boost::any& obj) {
        auto& values = boost::any_cast<const value_type&>(obj);
        size_t len = 0;
        for (auto&& val : values) {
            if (!val) {
                len += sizeof(uint32_t);
            } else {
                assert(val->size() <= std::numeric_limits<int32_t>::max());
                len += sizeof(uint32_t) + val->size();
            }
        }
        return len;
    }
    bool less(bytes_view b1, bytes_view b2) {
        return compare(b1, b2) < 0;
    }
    size_t hash(bytes_view v) {
        if (_byte_order_equal) {
            return std::hash<bytes_view>()(v);
        }
        auto t = _types.begin();
        size_t h = 0;
        for (auto&& value_opt : iter_items(v)) {
            if (value_opt) {
                h ^= (*t)->hash(*value_opt);
            }
            ++t;
        }
        return h;
    }
    int32_t compare(bytes_view b1, bytes_view b2) {
        if (is_byte_order_comparable()) {
            return compare_unsigned(b1, b2);
        }

        auto i1 = begin(b1);
        auto e1 = end(b1);
        auto i2 = begin(b2);
        auto e2 = end(b2);

        for (auto&& type : _types) {
            if (i1 == e1) {
                return i2 == e2 ? 0 : -1;
            }
            if (i2 == e2) {
                return 1;
            }
            auto v1 = *i1;
            auto v2 = *i2;
            if (bool(v1) != bool(v2)) {
                return v2 ? -1 : 1;
            }
            if (v1) {
                auto c = type->compare(*v1, *v2);
                if (c != 0) {
                    return c;
                }
            }
            ++i1;
            ++i2;
        }
        return 0;
    }
    bool is_byte_order_equal() const {
        return _byte_order_equal;
    }
    bool is_byte_order_comparable() const {
        // We're not byte order comparable because we encode component length as signed integer,
        // which is not byte order comparable.
        // TODO: make the length byte-order comparable by adding numeric_limits<int32_t>::min() when serializing
        return false;
    }
    bytes from_string(sstring_view s) {
        throw std::runtime_error("not implemented");
    }
    sstring to_string(const bytes& b) {
        throw std::runtime_error("not implemented");
    }
    /**
     * Returns true iff all components of 'prefix' are equal to corresponding
     * leading components of 'value'.
     *
     * The 'value' is assumed to be serialized using tuple_type<AllowPrefixes=false>
     */
    bool is_prefix_of(bytes_view prefix, bytes_view value) const {
        assert(AllowPrefixes);

        for (auto&& type : _types) {
            if (prefix.empty()) {
                return true;
            }
            assert(!value.empty());
            auto len1 = read_simple<int32_t>(prefix);
            auto len2 = read_simple<int32_t>(value);
            if ((len1 < 0) != (len2 < 0)) {
                // one is empty and another one is not
                return false;
            }
            if (len1 >= 0) {
                // both are not empty
                auto u_len1 = static_cast<uint32_t>(len1);
                auto u_len2 = static_cast<uint32_t>(len2);
                if (prefix.size() < u_len1 || value.size() < u_len2) {
                    throw marshal_exception();
                }
                if (!type->equal(bytes_view(prefix.begin(), u_len1), bytes_view(value.begin(), u_len2))) {
                    return false;
                }
                prefix.remove_prefix(u_len1);
                value.remove_prefix(u_len2);
            }
        }

        if (!prefix.empty() || !value.empty()) {
            throw marshal_exception();
        }

        return true;
    }
    // Retruns true iff given prefix has no missing components
    bool is_full(bytes_view v) const {
        assert(AllowPrefixes);
        return std::distance(begin(v), end(v)) == (ssize_t)_types.size();
    }
    void validate(bytes_view v) {
        // FIXME: implement
        warn(unimplemented::cause::VALIDATION);
    }
    bool equal(bytes_view v1, bytes_view v2) {
        if (_byte_order_equal) {
            return compare_unsigned(v1, v2) == 0;
        }
        // FIXME: call equal() on each component
        return compare(v1, v2) == 0;
    }
};

using tuple_prefix = tuple_type<true>;
