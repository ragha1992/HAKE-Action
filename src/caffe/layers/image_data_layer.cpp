#ifdef USE_OPENCV
#include <opencv2/core/core.hpp>

#include <fstream>   // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <string>
#include <utility>
#include <vector>

#include "caffe/data_transformer.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/layers/image_data_layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

namespace caffe {

template <typename Dtype>
ImageDataLayer<Dtype>::~ImageDataLayer<Dtype>() {
  this->StopInternalThread();
}

template <typename Dtype>
void ImageDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
                                           const vector<Blob<Dtype>*>& top) {
  const int new_height = this->layer_param_.image_data_param().new_height();
  const int new_width = this->layer_param_.image_data_param().new_width();
  const bool is_color = this->layer_param_.image_data_param().is_color();
  string root_folder = this->layer_param_.image_data_param().root_folder();

  // Initialise the label values.
  LABEL_VALUES[0] = USE_LABEL;
  LABEL_VALUES[1] = ignore_label_;

  // The separators used for label lists.
  char label_separator =
      this->layer_param_.image_data_param().label_separator().at(0);
  char label_list_separator =
      this->layer_param_.image_data_param().label_list_separator().at(0);

  CHECK((new_height == 0 && new_width == 0) ||
        (new_height > 0 && new_width > 0))
      << "Current implementation requires "
         "new_height and new_width to be set at the same time.";

  CHECK_NE(label_separator, label_list_separator)
      << "The separators "
         "specified for the labels and the list of labels may not be the same.";

  // Read the file with filenames and labels
  const string& source = this->layer_param_.image_data_param().source();
  LOG(INFO) << "Opening file " << source;
  std::ifstream infile(source.c_str());
  string filename;
  int label;

  int max_label_id = 0;
  bool is_multi_label = false;

  string line;
  while (std::getline(infile, line)) {
    std::istringstream iss(line);

    // Check if this is a comment line
    if (iss.peek() == '#') continue;

    iss >> filename;
    vector<vector<int> > labels(NUM_LABEL_LISTS);
    int total_labels = 0;

    while (iss.peek() == ' ') {
      iss.ignore();
    }


    for (int v = 0; v < labels.size(); ++v) {
      while (iss.peek() == ' ') {
        iss.ignore();
      }
      // Check for the label and ignore list separator.
      if (iss.peek() == label_list_separator) {
        iss.ignore();
        is_multi_label = true;
        continue;
      }
      while (iss >> label) {
        if (label > max_label_id) {
          max_label_id = label;
        }
        labels[v].push_back(label);
        total_labels++;

        // Check for the item separator.
        if (iss.peek() == label_separator) {
          iss.ignore();
        }
        // Check for the label and ignore list separator.
        if (iss.peek() == label_list_separator) {
          iss.ignore();
          break;
        }
      }
    }

    // The example is multi-label if more than one label has been specified per
    // line (this includes ignore labels).
    if (total_labels > 1) {
      is_multi_label = true;
    }

    lines_.push_back(std::make_pair(filename, labels));
  }

  if (is_multi_label) {
    num_labels_per_line_ = max_label_id + 1;
  } else {
    num_labels_per_line_ = 1;
  }

  if (this->layer_param_.image_data_param().shuffle()) {
    // randomly shuffle data
    LOG(INFO) << "Shuffling data";
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
    ShuffleImages();
  }
  LOG(INFO) << "A total of " << lines_.size() << " images.";

  lines_id_ = 0;
  // Check if we would need to randomly skip a few data points
  if (this->layer_param_.image_data_param().rand_skip()) {
    unsigned int skip =
        caffe_rng_rand() % this->layer_param_.image_data_param().rand_skip();
    LOG(INFO) << "Skipping first " << skip << " data points.";
    CHECK_GT(lines_.size(), skip) << "Not enough points to skip";
    lines_id_ = skip;
  }
  // Read an image, and use it to initialize the top blob.
  cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
                                    new_height, new_width, is_color);
  CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;
  // Use data_transformer to infer the expected blob shape from a cv_image.
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img);
  this->transformed_data_.Reshape(top_shape);
  // Reshape prefetch_data and top[0] according to the batch_size.
  const int batch_size = this->layer_param_.image_data_param().batch_size();
  CHECK_GT(batch_size, 0) << "Positive batch size required";
  top_shape[0] = batch_size;
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].data_.Reshape(top_shape);
  }
  top[0]->Reshape(top_shape);

  LOG(INFO) << "output data size: " << top[0]->num() << ","
            << top[0]->channels() << "," << top[0]->height() << ","
            << top[0]->width();
  // label
  vector<int> label_shape(2);
  label_shape[0] = batch_size;
  label_shape[1] = num_labels_per_line_;
  top[1]->Reshape(label_shape);
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].label_.Reshape(label_shape);
  }
}

template <typename Dtype>
void ImageDataLayer<Dtype>::ShuffleImages() {
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  shuffle(lines_.begin(), lines_.end(), prefetch_rng);
}

// This function is called on prefetch thread
template <typename Dtype>
void ImageDataLayer<Dtype>::load_batch(Batch<Dtype>* batch) {
  CPUTimer batch_timer;
  batch_timer.Start();
  double read_time = 0;
  double trans_time = 0;
  CPUTimer timer;
  CHECK(batch->data_.count());
  CHECK(this->transformed_data_.count());
  ImageDataParameter image_data_param = this->layer_param_.image_data_param();
  const int batch_size = image_data_param.batch_size();
  const int new_height = image_data_param.new_height();
  const int new_width = image_data_param.new_width();
  const bool is_color = image_data_param.is_color();
  string root_folder = image_data_param.root_folder();

  // Reshape according to the first image of each batch
  // on single input batches allows for inputs of varying dimension.
  cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
                                    new_height, new_width, is_color);
  while(!cv_img.data){
    std::cout<<"Could not load " << lines_[lines_id_].first;
    lines_id_++;
    if (lines_id_ >= lines_.size()) {
      // We have reached the end. Restart from the first.
      DLOG(INFO) << "Restarting data prefetching from start.";
      lines_id_ = 0;
      if (this->layer_param_.image_data_param().shuffle()) {
        ShuffleImages();
      }
    }
    cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
                                    new_height, new_width, is_color);
  }
  // Use data_transformer to infer the expected blob shape from a cv_img.
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img);
  this->transformed_data_.Reshape(top_shape);
  // Reshape batch according to the batch_size.
  top_shape[0] = batch_size;
  batch->data_.Reshape(top_shape);

  Dtype* prefetch_data = batch->data_.mutable_cpu_data();
  Dtype* prefetch_label = batch->label_.mutable_cpu_data();

  // Init the labels to 0.
  caffe_set(batch_size * num_labels_per_line_, Dtype(0), prefetch_label);

  // datum scales
  const int lines_size = lines_.size();
  for (int item_id = 0; item_id < batch_size; ++item_id) {
    // get a blob
    timer.Start();
    CHECK_GT(lines_size, lines_id_);
    cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
                                      new_height, new_width, is_color);
  while(!cv_img.data){
    std::cout<<"Could not load " << lines_[lines_id_].first;
    lines_id_++;
    if (lines_id_ >= lines_.size()) {
      // We have reached the end. Restart from the first.
      DLOG(INFO) << "Restarting data prefetching from start.";
      lines_id_ = 0;
      if (this->layer_param_.image_data_param().shuffle()) {
        ShuffleImages();
      }
    }
    cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
                                    new_height, new_width, is_color);
  }
    read_time += timer.MicroSeconds();
    timer.Start();
    // Apply transformations (mirror, crop...) to the image
    int offset = batch->data_.offset(item_id);
    this->transformed_data_.set_cpu_data(prefetch_data + offset);
    this->data_transformer_->Transform(cv_img, &(this->transformed_data_));
    trans_time += timer.MicroSeconds();

    if (num_labels_per_line_ == 1) {
      prefetch_label[item_id] = lines_[lines_id_].second[0][0];
    } else {
      int label_offset = batch->label_.offset(item_id);
      // Set the labels and ignore label values.
      for (int v = 0; v < NUM_LABEL_LISTS; ++v) {
        for (int l = 0; l < lines_[lines_id_].second[v].size(); ++l) {
          int label_id = lines_[lines_id_].second[v][l];
          Dtype current_value = prefetch_label[label_offset + label_id];
          bool OK = (current_value == 0 || current_value == LABEL_VALUES[v]);

          //CHECK(OK) << "label " << label_id << " on line " << lines_id_
          //         << " already set as: " << current_value<< ")";

          prefetch_label[label_offset + label_id] = LABEL_VALUES[v];
        }
      }
    }

    // go to the next iter
    lines_id_++;
    if (lines_id_ >= lines_size) {
      // We have reached the end. Restart from the first.
      DLOG(INFO) << "Restarting data prefetching from start.";
      lines_id_ = 0;
      if (this->layer_param_.image_data_param().shuffle()) {
        ShuffleImages();
      }
    }
  }
  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
  DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
  DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

INSTANTIATE_CLASS(ImageDataLayer);
REGISTER_LAYER_CLASS(ImageData);

}  // namespace caffe
#endif  // USE_OPENCV