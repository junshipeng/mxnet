/*!
 *  Copyright (c) 2015 by Contributors
 * \file iter_batch_proc-inl.hpp
 * \brief definition of preprocessing iterators that takes an iterator and do some preprocessing
 * \author Tianqi Chen, Tianjun Xiao
 */
#ifndef MXNET_IO_ITER_BATCH_H_
#define MXNET_IO_ITER_BATCH_H_

#include <mxnet/io.h>
#include <mxnet/base.h>
#include <dmlc/logging.h>
#include <mshadow/tensor.h>
#include <utility>
#include <string>
#include <vector>

namespace mxnet {
namespace io {
// Batch parameters
struct BatchParam : public dmlc::Parameter<BatchParam> {
  /*! \brief label width */
  index_t batch_size;
  /*! \brief input shape */
  TShape input_shape;
  /*! \brief label width */
  index_t label_width;
  /*! \brief use round roubin to handle overflow batch */
  bool round_batch;
  /*! \brief skip read */
  bool test_skipread;
  /*! \brief silent */
  bool silent;
  // declare parameters
  DMLC_DECLARE_PARAMETER(BatchParam) {
    DMLC_DECLARE_FIELD(batch_size)
        .describe("Batch size.");
    index_t input_shape_default[] = {3, 224, 224};
    DMLC_DECLARE_FIELD(input_shape)
        .set_default(TShape(input_shape_default, input_shape_default + 3))
        .set_expect_ndim(3).enforce_nonzero()
        .describe("Input shape of the neural net");
    DMLC_DECLARE_FIELD(label_width).set_default(1)
        .describe("Label width.");
    DMLC_DECLARE_FIELD(round_batch).set_default(true)
        .describe("Use round robin to handle overflow batch.");
    DMLC_DECLARE_FIELD(test_skipread).set_default(false)
        .describe("Skip read for testing.");
    DMLC_DECLARE_FIELD(silent).set_default(false)
        .describe("Whether to print batch information.");
  }
};

/*! \brief create a batch iterator from single instance iterator */
class BatchAdaptIter: public IIterator<DataBatch> {
 public:
  explicit BatchAdaptIter(IIterator<DataInst> *base): base_(base), num_overflow_(0) {}
  virtual ~BatchAdaptIter(void) {
    delete base_;
    FreeSpaceDense();
  }
  virtual void Init(const std::vector<std::pair<std::string, std::string> >& kwargs) {
    std::vector<std::pair<std::string, std::string> > kwargs_left;
    // init batch param, it could have similar param with
    kwargs_left = param_.InitAllowUnknown(kwargs);
    // init base iterator
    base_->Init(kwargs);
    data_shape_[1] = param_.input_shape[0];
    data_shape_[2] = param_.input_shape[1];
    data_shape_[3] = param_.input_shape[2];
    data_shape_[0] = param_.batch_size;
    AllocSpaceDense(false);
  }
  virtual void BeforeFirst(void) {
    if (param_.round_batch == 0 || num_overflow_ == 0) {
      // otherise, we already called before first
      base_->BeforeFirst();
    } else {
      num_overflow_ = 0;
    }
    head_ = 1;
  }
  virtual bool Next(void) {
    out_.num_batch_padd = 0;

    // skip read if in head version
    if (param_.test_skipread != 0 && head_ == 0)
        return true;
    else
        this->head_ = 0;

    // if overflow from previous round, directly return false, until before first is called
    if (num_overflow_ != 0) return false;
    index_t top = 0;

    while (base_->Next()) {
      const DataInst& d = base_->Value();
      mshadow::Copy(label[top], d.data[1].get<mshadow::cpu, 1, float>());
      out_.inst_index[top] = d.index;
      mshadow::Copy(data[top], d.data[0].get<mshadow::cpu, 3, float>());

      if (++ top >= param_.batch_size) {
        out_.data[0] = TBlob(data);
        out_.data[1] = TBlob(label);
        return true;
      }
    }
    if (top != 0) {
      if (param_.round_batch != 0) {
        num_overflow_ = 0;
        base_->BeforeFirst();
        for (; top < param_.batch_size; ++top, ++num_overflow_) {
          CHECK(base_->Next()) << "number of input must be bigger than batch size";
          const DataInst& d = base_->Value();
          mshadow::Copy(label[top], d.data[1].get<mshadow::cpu, 1, float>());
          out_.inst_index[top] = d.index;
          mshadow::Copy(data[top], d.data[0].get<mshadow::cpu, 3, float>());
        }
        out_.num_batch_padd = num_overflow_;
      } else {
        out_.num_batch_padd = param_.batch_size - top;
      }
      out_.data[0] = TBlob(data);
      out_.data[1] = TBlob(label);
      return true;
    }
    return false;
  }
  virtual const DataBatch &Value(void) const {
    CHECK(head_ == 0) << "must call Next to get value";
    return out_;
  }

 private:
  /*! \brief batch parameters */
  BatchParam param_;
  /*! \brief base iterator */
  IIterator<DataInst> *base_;
  /*! \brief output data */
  DataBatch out_;
  /*! \brief on first */
  int head_;
  /*! \brief number of overflow instances that readed in round_batch mode */
  int num_overflow_;
  /*! \brief label information of the data*/
  mshadow::Tensor<mshadow::cpu, 2> label;
  /*! \brief content of dense data, if this DataBatch is dense */
  mshadow::Tensor<mshadow::cpu, 4> data;
  /*! \brief data shape */
  mshadow::Shape<4> data_shape_;
  // Functions that allocate and free tensor space
  inline void AllocSpaceDense(bool pad = false) {
    data = mshadow::NewTensor<mshadow::cpu>(data_shape_, 0.0f, pad);
    mshadow::Shape<2> lshape = mshadow::Shape2(param_.batch_size, param_.label_width);
    label = mshadow::NewTensor<mshadow::cpu>(lshape, 0.0f, pad);
    out_.inst_index = new unsigned[param_.batch_size];
    out_.batch_size = param_.batch_size;
    out_.data.resize(2);
  }
  /*! \brief auxiliary function to free space, if needed, dense only */
  inline void FreeSpaceDense(void) {
    if (label.dptr_ != NULL) {
      delete [] out_.inst_index;
      mshadow::FreeSpace(&label);
      mshadow::FreeSpace(&data);
      label.dptr_ = NULL;
    }
  }
};  // class BatchAdaptIter
}  // namespace io
}  // namespace mxnet
#endif  // MXNET_IO_ITER_BATCH_H_