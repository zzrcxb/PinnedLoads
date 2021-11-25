#ifndef PERF_COUNTER_HH
#define PERF_COUNTER_HH

#include <iostream>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include "base/types.hh"
#include "cpu/global_utils.hh"


template <typename T>
struct Statistian {
    std::string name;

    Statistian() {
        cnt = 0;
        sum = 0;
        max = std::numeric_limits<T>::min();
        min = std::numeric_limits<T>::max();
    }

    Statistian(std::string name) : name(name) {
        Statistian();
    }

    Statistian(std::string name, T min, T max)
        : name(name), min(min), max(max) {
        cnt = 0;
        sum = 0;
    }

    void setName(std::string name) {
        this->name = name;
    }

    void update(T val) {
        cnt++;
        max = val > max ? val : max;
        min = val < min ? val : min;
        sum += val;
    }

    void update(bool b) {
        if (b)
            update((T)1);
        else
            update((T)0);
    }

    uint64_t getCnt() const { return cnt; }

    T getMax() const {
        if (cnt)
            return max;
        else
            return 0;
    }

    T getMin() const {
        if (cnt)
            return min;
        else
            return 0;
    }

    T getSum() const { return sum; }

    double getAvg() const {
        if (cnt > 0)
            return ((double)sum) / ((double)cnt);
        else
            return 0.0;
    }

    double getRatio() const {
        return getAvg();
    }

    void dumpHeader(std::ostream &outfile) const {
        outfile << name << "_cnt@weight,"
                << name << "_avg@weight,"
                << name << "_min@min,"
                << name << "_max@max,|,";
    }

    void dump(std::ostream &outfile) const {
        outfile << getCnt() << ","
                << getAvg() << ","
                << getMin() << ","
                << getMax() << ",|,";
    }

   private:
    uint64_t cnt;
    T max, min, sum;
};


struct PerfCounter {
    struct PerfRecord {
        ContextID ctx;
        Addr pc;
        Addr microPC;
        char type;

        size_t total_cnt, squashed_cnt, committed_cnt;
        Statistian<uint32_t> esp_delayed, osp_delayed;

        PerfRecord(ContextID ctx, Addr pc, Addr microPC, char type) :
            ctx(ctx), pc(pc), microPC(microPC), type(type), total_cnt(0),
            squashed_cnt(0), committed_cnt(0) {
            esp_delayed.setName("esp_delayed");
            osp_delayed.setName("osp_delayed");
        }

        void update(bool committed, uint32_t _esp_delayed, uint32_t _osp_delayed) {
            total_cnt += 1;
            if (committed) {
                committed_cnt += 1;
                esp_delayed.update(_esp_delayed);
                osp_delayed.update(_osp_delayed);
            } else {
                squashed_cnt += 1;
            }
        }

        void dumpHeader(std::ostream &outfile) const {
            outfile << "tid,PC@uOp(type),|,total,committed,squashed,|,";
            esp_delayed.dumpHeader(outfile);
            osp_delayed.dumpHeader(outfile);
        }

        void dump(std::ostream &outfile) const {
            outfile << ctx << ","
                    << std::hex << "0x" << pc << "@" << std::dec << microPC
                    << "(" << type << "),|,"
                    << total_cnt << ","
                    << committed_cnt << ","
                    << squashed_cnt << ",|,";
            esp_delayed.dump(outfile);
            osp_delayed.dump(outfile);
        }
    };

    typedef std::shared_ptr<PerfRecord> PerfRecord_p;

    PerfCounter() = default;

    void update(ContextID ctx, Addr pc, Addr microPC, char type, bool committed,
                uint32_t esp_delayed, uint32_t osp_delayed) {
        PerfKey key(ctx, pc, microPC);

        if (!IN_MAP(key, _records)) {
            _records[key].reset(new PerfRecord(ctx, pc, microPC, type));
        }
        _records.at(key)->update(committed, esp_delayed, osp_delayed);
    }

    void dumpAll(std::ostream &outfile, uint64_t threshold=0) {
        bool headerDumped = false;
        for (auto &p : _records) {
            if (!headerDumped) {
                p.second->dumpHeader(outfile);
                outfile << std::endl;
                headerDumped = true;
            }

            if (p.second->committed_cnt >= threshold) {
                p.second->dump(outfile);
                outfile << std::endl;
            }
        }
    }

  private:

    struct PerfKey {
        ContextID ctx;
        Addr pc;
        Addr microPC;

        PerfKey(ContextID _ctx, Addr _pc, Addr _microPC) :
               ctx(_ctx), pc(_pc), microPC(_microPC) {}

        bool operator==(const PerfKey &other) const {
            return pc == other.pc && microPC == other.microPC && ctx == other.ctx;
        }
    };

    struct PerfKeyHasher {
        size_t operator()(const PerfKey &key) const {
            return (key.pc << 10) & (key.microPC << 4) & key.ctx;
        }
    };

    // TODO: PerfKey has potential performance issue
    std::unordered_map<PerfKey, PerfRecord_p, PerfKeyHasher> _records;
};

#endif
