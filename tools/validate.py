# Copyright 2018 The MACE Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import sys
import os
import os.path
import numpy as np
import six

import common

# Validation Flow:
# 1. Generate input data
# 2. Use mace_run to run model on phone.
# 3. adb pull the result.
# 4. Compare output data of mace and tf
#    python validate.py --model_file tf_model_opt.pb \
#        --input_file input_file \
#        --mace_out_file output_file \
#        --input_node input_node \
#        --output_node output_node \
#        --input_shape 1,64,64,3 \
#        --output_shape 1,64,64,2
#        --validation_threshold 0.995

VALIDATION_MODULE = 'VALIDATION'


def load_data(file, data_type='float32'):
    if os.path.isfile(file):
        if data_type == 'float32' or data_type == 'float16' or \
                data_type == 'bfloat16':
            return np.fromfile(file=file, dtype=np.float32)
        elif data_type == 'int32':
            return np.fromfile(file=file, dtype=np.int32)
    return np.empty([0])


def calculate_sqnr(expected, actual):
    noise = expected - actual

    def power_sum(xs):
        return sum([x * x for x in xs])

    signal_power_sum = power_sum(expected)
    noise_power_sum = power_sum(noise)
    return signal_power_sum / (noise_power_sum + 1e-15)


def calculate_similarity(u, v, data_type=np.float64):
    if u.dtype is not data_type:
        u = u.astype(data_type)
    if v.dtype is not data_type:
        v = v.astype(data_type)
    u_norm = np.linalg.norm(u)
    v_norm = np.linalg.norm(v)
    norm = u_norm * v_norm
    if norm == 0:
        if u_norm == 0 and v_norm == 0:
            return data_type(1)
        else:
            return data_type(0)
    else:
        return np.dot(u, v) / norm


def calculate_pixel_accuracy(out_value, mace_out_value,
                             output_shape, output_data_format):
    out_value = out_value.reshape(output_shape)
    mace_out_value = mace_out_value.reshape(output_shape)
    if len(output_shape) == 4 and output_data_format == common.DataFormat.NCHW:
        out_value = out_value.transpose([0, 2, 3, 1])
        mace_out_value = mace_out_value.transpose([0, 2, 3, 1])
    if len(out_value.shape) < 2:
        return 1.0
    out_value = out_value.reshape((-1, out_value.shape[-1]))
    batches = out_value.shape[0]
    classes = out_value.shape[1]
    mace_out_value = mace_out_value.reshape((batches, classes))
    correct_count = 0
    for i in range(batches):
        if np.argmax(out_value[i]) == np.argmax(mace_out_value[i]):
            correct_count += 1
    return 1.0 * correct_count / batches


def compare_output(platform, device_type, output_name, mace_out_value,
                   out_value, validation_threshold, log_file,
                   output_shape, output_data_format):
    if mace_out_value.size != 0:
        pixel_accuracy = calculate_pixel_accuracy(out_value, mace_out_value,
                                                  output_shape,
                                                  output_data_format)
        out_value = out_value.reshape(-1)
        mace_out_value = mace_out_value.reshape(-1)
        assert len(out_value) == len(mace_out_value)
        sqnr = calculate_sqnr(out_value, mace_out_value)
        similarity = calculate_similarity(out_value, mace_out_value)
        common.MaceLogger.summary(
            output_name + ' MACE VS ' + platform.upper()
            + ' similarity: ' + str(similarity) + ' , sqnr: ' + str(sqnr)
            + ' , pixel_accuracy: ' + str(pixel_accuracy))
        if log_file:
            if not os.path.exists(log_file):
                with open(log_file, 'w') as f:
                    f.write('output_name,similarity,sqnr,pixel_accuracy\n')
            summary = '{output_name},{similarity},{sqnr},{pixel_accuracy}\n'\
                .format(output_name=output_name,
                        similarity=similarity,
                        sqnr=sqnr,
                        pixel_accuracy=pixel_accuracy)
            with open(log_file, "a") as f:
                f.write(summary)
        elif similarity > validation_threshold:
            common.MaceLogger.summary(
                common.StringFormatter.block("Similarity Test Passed"))
        else:
            common.MaceLogger.error(
                "", common.StringFormatter.block("Similarity Test Failed"))
    else:
        common.MaceLogger.error(
            "", common.StringFormatter.block(
                "Similarity Test failed because of empty output"))


def normalize_tf_tensor_name(name):
    if name.find(':') == -1:
        return name + ':0'
    else:
        return name


def get_real_out_value_shape_df(platform, mace_out_value, output_shape,
                                output_data_format):
    real_output_shape = output_shape
    real_output_data_format = output_data_format
    if len(output_shape) == 4:
        # These platforms use NHWC, if MACE's output is NCHW,
        # transpose the output of MACE from NCHW to NHWC.
        if output_data_format == common.DataFormat.NCHW and \
                platform.lower() in ["tensorflow", "keras"]:
            mace_out_value = mace_out_value.reshape(output_shape)\
                .transpose((0, 2, 3, 1))
            real_output_shape = list(mace_out_value.shape)
            real_output_data_format = common.DataFormat.NHWC
        # These platforms use NCHW, if MACE's output is NHWC,
        # transpose the output of MACE from NHWC to NCHW.
        elif output_data_format == common.DataFormat.NHWC and \
                platform.lower() in ["pytorch", "caffe", "onnx",
                                     "megengine"]:
            mace_out_value = mace_out_value.reshape(output_shape)\
                .transpose((0, 3, 1, 2))
            real_output_shape = list(mace_out_value.shape)
            real_output_data_format = common.DataFormat.NCHW
    return mace_out_value, real_output_shape, real_output_data_format


def validate_with_file(platform, device_type,
                       output_names, output_shapes,
                       mace_out_file, validation_outputs_data,
                       validation_threshold, log_file,
                       output_data_formats):
    for i in range(len(output_names)):
        if validation_outputs_data[i].startswith("http://") or \
                validation_outputs_data[i].startswith("https://"):
            validation_file_name = common.formatted_file_name(
                mace_out_file, output_names[i] + '_validation')
            six.moves.urllib.request.urlretrieve(validation_outputs_data[i],
                                                 validation_file_name)
        else:
            validation_file_name = validation_outputs_data[i]
        value = load_data(validation_file_name)
        out_shape = output_shapes[i]
        output_file_name = common.formatted_file_name(
            mace_out_file, output_names[i])
        mace_out_value = load_data(output_file_name)
        mace_out_value, real_output_shape, real_output_data_format = \
            get_real_out_value_shape_df(platform,
                                        mace_out_value,
                                        output_shapes[i],
                                        output_data_formats[i])
        compare_output(platform, device_type, output_names[i], mace_out_value,
                       value, validation_threshold, log_file,
                       real_output_shape, real_output_data_format)


def validate_tf_model(platform, device_type, model_file,
                      input_file, mace_out_file,
                      input_names, input_shapes, input_data_formats,
                      output_names, output_shapes, output_data_formats,
                      validation_threshold, input_data_types,
                      output_data_types, log_file):
    import tensorflow as tf
    if not os.path.isfile(model_file):
        common.MaceLogger.error(
            VALIDATION_MODULE,
            "Input graph file '" + model_file + "' does not exist!")

    tf.reset_default_graph()
    input_graph_def = tf.GraphDef()
    with open(model_file, "rb") as f:
        data = f.read()
        input_graph_def.ParseFromString(data)
        # delete explicit_paddings
        for node in input_graph_def.node:
            name = str(node.name).split('/')[-1]
            if name == 'depthwise_Fold' or name == 'depthwise':
                if 'explicit_paddings' in node.attr:
                    del node.attr['explicit_paddings']
        tf.import_graph_def(input_graph_def, name="")

        with tf.Session() as session:
            with session.graph.as_default() as graph:
                tf.import_graph_def(input_graph_def, name="")
                input_dict = {}
                for i in range(len(input_names)):
                    input_value = load_data(
                        common.formatted_file_name(input_file, input_names[i]),
                        input_data_types[i])
                    input_value = input_value.reshape(input_shapes[i])
                    if input_data_formats[i] == common.DataFormat.NCHW and\
                            len(input_shapes[i]) == 4:
                        input_value = input_value.transpose((0, 2, 3, 1))
                    elif input_data_formats[i] == common.DataFormat.OIHW and \
                            len(input_shapes[i]) == 4:
                        # OIHW -> HWIO
                        input_value = input_value.transpose((2, 3, 1, 0))
                    input_node = graph.get_tensor_by_name(
                        normalize_tf_tensor_name(input_names[i]))
                    input_dict[input_node] = input_value

                output_nodes = []
                for name in output_names:
                    output_nodes.extend(
                        [graph.get_tensor_by_name(
                            normalize_tf_tensor_name(name))])
                output_values = session.run(output_nodes, feed_dict=input_dict)
                for i in range(len(output_names)):
                    output_file_name = common.formatted_file_name(
                        mace_out_file, output_names[i])
                    mace_out_value = load_data(output_file_name,
                                               output_data_types[i])
                    mace_out_value, real_out_shape, real_out_data_format = \
                        get_real_out_value_shape_df(platform,
                                                    mace_out_value,
                                                    output_shapes[i],
                                                    output_data_formats[i])
                    compare_output(platform, device_type, output_names[i],
                                   mace_out_value, output_values[i],
                                   validation_threshold, log_file,
                                   real_out_shape, real_out_data_format)


def validate_pytorch_model(platform, device_type, model_file,
                           input_file, mace_out_file,
                           input_names, input_shapes, input_data_formats,
                           output_names, output_shapes, output_data_formats,
                           validation_threshold, input_data_types,
                           output_data_types, log_file):
    import torch
    loaded_model = torch.jit.load(model_file)
    pytorch_inputs = []
    for i in range(len(input_names)):
        input_value = load_data(
            common.formatted_file_name(input_file, input_names[i]),
            input_data_types[i])
        input_value = input_value.reshape(input_shapes[i])
        if input_data_formats[i] == common.DataFormat.NHWC and \
                len(input_shapes[i]) == 4:
            input_value = input_value.transpose((0, 3, 1, 2))
        input_value = torch.from_numpy(input_value)
        pytorch_inputs.append(input_value)
    with torch.no_grad():
        pytorch_outputs = loaded_model(*pytorch_inputs)

    if isinstance(pytorch_outputs, torch.Tensor):
        pytorch_outputs = [pytorch_outputs]
    else:
        if not isinstance(pytorch_outputs, (list, tuple)):
            print('return type {} unsupported yet'.format(
                type(pytorch_outputs)))
            sys.exit(1)
    for i in range(len(output_names)):
        value = pytorch_outputs[i].numpy()
        output_file_name = common.formatted_file_name(
            mace_out_file, output_names[i])
        mace_out_value = load_data(output_file_name, output_data_types[i])
        mace_out_value, real_output_shape, real_output_data_format = \
            get_real_out_value_shape_df(platform,
                                        mace_out_value,
                                        output_shapes[i],
                                        output_data_formats[i])
        compare_output(platform, device_type, output_names[i], mace_out_value,
                       value, validation_threshold, log_file,
                       real_output_shape, real_output_data_format)


def validate_caffe_model(platform, device_type, model_file, input_file,
                         mace_out_file, weight_file,
                         input_names, input_shapes, input_data_formats,
                         output_names, output_shapes, output_data_formats,
                         validation_threshold, log_file):
    os.environ['GLOG_minloglevel'] = '1'  # suprress Caffe verbose prints
    import caffe
    if not os.path.isfile(model_file):
        common.MaceLogger.error(
            VALIDATION_MODULE,
            "Input graph file '" + model_file + "' does not exist!")
    if not os.path.isfile(weight_file):
        common.MaceLogger.error(
            VALIDATION_MODULE,
            "Input weight file '" + weight_file + "' does not exist!")

    caffe.set_mode_cpu()

    net = caffe.Net(model_file, caffe.TEST, weights=weight_file)

    for i in range(len(input_names)):
        input_value = load_data(
            common.formatted_file_name(input_file, input_names[i]))
        input_value = input_value.reshape(input_shapes[i])
        if input_data_formats[i] == common.DataFormat.NHWC and \
                len(input_shapes[i]) == 4:
            input_value = input_value.transpose((0, 3, 1, 2))
        input_blob_name = input_names[i]
        try:
            if input_names[i] in net.top_names:
                input_blob_name = net.top_names[input_names[i]][0]
        except ValueError:
            pass
        new_shape = input_value.shape
        net.blobs[input_blob_name].reshape(*new_shape)
        for index in range(input_value.shape[0]):
            net.blobs[input_blob_name].data[index] = input_value[index]

    net.forward()

    for i in range(len(output_names)):
        value = net.blobs[output_names[i]].data
        output_file_name = common.formatted_file_name(
            mace_out_file, output_names[i])
        mace_out_value = load_data(output_file_name)
        mace_out_value, real_output_shape, real_output_data_format = \
            get_real_out_value_shape_df(platform,
                                        mace_out_value,
                                        output_shapes[i],
                                        output_data_formats[i])
        compare_output(platform, device_type, output_names[i], mace_out_value,
                       value, validation_threshold, log_file,
                       real_output_shape, real_output_data_format)


# Remove annoying warning from onnxruntime.
def remove_initializer_from_input(model):
    if model.ir_version < 4:
        print("Model with ir_version below 4 requires to include initilizer in"
              " graph input")
        return

    inputs = model.graph.input
    name_to_input = {}
    for input in inputs:
        name_to_input[input.name] = input

    for initializer in model.graph.initializer:
        if initializer.name in name_to_input:
            inputs.remove(name_to_input[initializer.name])


def validate_onnx_model(platform, device_type, model_file,
                        input_file, mace_out_file,
                        input_names, input_shapes, input_data_formats,
                        output_names, output_shapes, output_data_formats,
                        validation_threshold, input_data_types,
                        output_data_types, backend, log_file):
    print("validate on onnxruntime.")
    import onnx
    import onnxruntime as onnxrt

    if not os.path.isfile(model_file):
        common.MaceLogger.error(
            VALIDATION_MODULE,
            "Input graph file '" + model_file + "' does not exist!")

    model = onnx.load(model_file)
    remove_initializer_from_input(model)
    model_outputs = set()
    for output in model.graph.output:
        model_outputs.add(output.name)
    for output_name in output_names:
        if output_name not in model_outputs:
            layer_value_info = onnx.helper.ValueInfoProto()
            layer_value_info.name = output_name
            model.graph.output.append(layer_value_info)

    input_dict = {}
    for i in range(len(input_names)):
        input_value = load_data(common.formatted_file_name(input_file,
                                                           input_names[i]),
                                input_data_types[i])
        input_value = input_value.reshape(input_shapes[i])
        if input_data_formats[i] == common.DataFormat.NHWC and \
                len(input_shapes[i]) == 4:
            input_value = input_value.transpose((0, 3, 1, 2))
        input_dict[input_names[i]] = input_value

    sess = onnxrt.InferenceSession(model.SerializeToString())
    output_values = sess.run(output_names, input_dict)

    for i in range(len(output_names)):
        value = output_values[i].flatten()
        output_file_name = common.formatted_file_name(mace_out_file,
                                                      output_names[i])
        mace_out_value = load_data(output_file_name, output_data_types[i])
        mace_out_value, real_output_shape, real_output_data_format = \
            get_real_out_value_shape_df(platform,
                                        mace_out_value,
                                        output_shapes[i],
                                        output_data_formats[i])
        compare_output(platform, device_type, output_names[i],
                       mace_out_value, value,
                       validation_threshold, log_file,
                       real_output_shape, real_output_data_format)


def validate_megengine_model(platform, device_type, model_file, input_file,
                             mace_out_file, input_names, input_shapes,
                             input_data_formats, output_names, output_shapes,
                             output_data_formats, validation_threshold,
                             input_data_types, output_data_types, log_file):
    import megengine._internal as mgb

    if not os.path.isfile(model_file):
        common.MaceLogger.error(
            VALIDATION_MODULE,
            "Input graph file '" + model_file + "' does not exist!",
        )

    feed_inputs = []
    for i in range(len(input_names)):
        input_value = load_data(
            common.formatted_file_name(input_file, input_names[i]),
            input_data_types[i])
        input_value = input_value.reshape(input_shapes[i])
        if (input_data_formats[i] == common.DataFormat.NHWC and
                len(input_shapes[i]) == 4):
            input_value = input_value.transpose((0, 3, 1, 2))
        feed_inputs.append(input_value)

    cg, _, outputs = mgb.load_comp_graph_from_file(model_file)
    inputs = mgb.cgtools.get_dep_vars(outputs, "Host2DeviceCopy")
    inputs = sorted(inputs, key=lambda i: i.name)
    outputs = list(map(mgb.copy_output, outputs))
    if len(outputs) == 1:
        (outputs,) = outputs
    func = cg.compile(inputs, outputs)

    mge_output_value = func(*feed_inputs)

    for i in range(len(output_names)):
        output_file_name = \
            common.formatted_file_name(mace_out_file, output_names[i])
        mace_out_value = load_data(output_file_name, output_data_types[i])
        mace_out_value, real_output_shape, real_output_data_format = \
            get_real_out_value_shape_df(platform,
                                        mace_out_value,
                                        output_shapes[i],
                                        output_data_formats[i])
        compare_output(platform, device_type, output_names[i], mace_out_value,
                       mge_output_value, validation_threshold, log_file,
                       real_output_shape, real_output_data_format)


def validate_keras_model(platform, device_type, model_file,
                         input_file, mace_out_file,
                         input_names, input_shapes, input_data_formats,
                         output_names, output_shapes, output_data_formats,
                         validation_threshold, input_data_types,
                         output_data_types, log_file):
    from tensorflow import keras
    import tensorflow_model_optimization as tfmot

    if not os.path.isfile(model_file):
        common.MaceLogger.error(
            VALIDATION_MODULE,
            "Input model file '" + model_file + "' does not exist!")

    with tfmot.quantization.keras.quantize_scope():
        keras_model = keras.models.load_model(model_file, compile=False)

        input = []
        for i in range(len(input_names)):
            input_value = load_data(
                common.formatted_file_name(input_file, input_names[i]),
                input_data_types[i])
            input_value = input_value.reshape(input_shapes[i])
            if input_data_formats[i] == common.DataFormat.NCHW and \
                    len(input_shapes[i]) == 4:
                input_value = input_value.transpose((0, 2, 3, 1))
            elif input_data_formats[i] == common.DataFormat.OIHW and \
                    len(input_shapes[i]) == 4:
                # OIHW -> HWIO
                input_value = input_value.transpose((2, 3, 1, 0))
            input.append(input_value)

        output_values = keras_model.predict(input)

        for i in range(len(output_names)):
            output_file_name = common.formatted_file_name(
                mace_out_file, output_names[i])
            mace_out_value = load_data(output_file_name, output_data_types[i])
            mace_out_value, real_output_shape, real_output_data_format = \
                get_real_out_value_shape_df(platform,
                                            mace_out_value,
                                            output_shapes[i],
                                            output_data_formats[i])
            compare_output(platform, device_type, output_names[i],
                           mace_out_value, output_values[i],
                           validation_threshold, log_file,
                           real_output_shape, real_output_data_format)


def validate(platform, model_file, weight_file, input_file, mace_out_file,
             device_type, input_shape, output_shape, input_data_format_str,
             output_data_format_str, input_node, output_node,
             validation_threshold, input_data_type, output_data_type, backend,
             validation_outputs_data, log_file):
    input_names = [name for name in input_node.split(',')]
    input_shape_strs = [shape for shape in input_shape.split(':')]
    input_shapes = [[int(x) for x in common.split_shape(shape)]
                    for shape in input_shape_strs]
    output_shape_strs = [shape for shape in output_shape.split(':')]
    output_shapes = [[int(x) for x in common.split_shape(shape)]
                     for shape in output_shape_strs]
    input_data_formats = [df for df in input_data_format_str.split(',')]
    output_data_formats = [df for df in output_data_format_str.split(',')]
    if input_data_type:
        input_data_types = [data_type
                            for data_type in input_data_type.split(',')]
    else:
        input_data_types = ['float32'] * len(input_names)
    if output_data_type:
        output_data_types = [data_type
                             for data_type in output_data_type.split(',')]
    else:
        output_data_types = ['float32'] * len(output_shapes)
    output_names = [name for name in output_node.split(',')]
    assert len(input_names) == len(input_shapes)
    if not isinstance(validation_outputs_data, list):
        if os.path.isfile(validation_outputs_data) or \
                validation_outputs_data.startswith("http://") or \
                validation_outputs_data.startswith("https://"):
            validation_outputs = [validation_outputs_data]
        else:
            validation_outputs = []
    else:
        validation_outputs = validation_outputs_data
    if validation_outputs:
        validate_with_file(platform, device_type, output_names, output_shapes,
                           mace_out_file, validation_outputs,
                           validation_threshold, log_file,
                           output_data_formats)
    elif platform == 'tensorflow':
        validate_tf_model(platform, device_type,
                          model_file, input_file, mace_out_file,
                          input_names, input_shapes, input_data_formats,
                          output_names, output_shapes, output_data_formats,
                          validation_threshold, input_data_types,
                          output_data_types, log_file)
    elif platform == 'pytorch':
        validate_pytorch_model(platform, device_type,
                               model_file, input_file, mace_out_file,
                               input_names, input_shapes, input_data_formats,
                               output_names, output_shapes,
                               output_data_formats, validation_threshold,
                               input_data_types, output_data_types, log_file)
    elif platform == 'caffe':
        validate_caffe_model(platform, device_type, model_file,
                             input_file, mace_out_file, weight_file,
                             input_names, input_shapes, input_data_formats,
                             output_names, output_shapes, output_data_formats,
                             validation_threshold, log_file)
    elif platform == 'onnx':
        validate_onnx_model(platform, device_type, model_file,
                            input_file, mace_out_file,
                            input_names, input_shapes, input_data_formats,
                            output_names, output_shapes, output_data_formats,
                            validation_threshold, input_data_types,
                            output_data_types, backend, log_file)
    elif platform == 'megengine':
        validate_megengine_model(platform, device_type, model_file,
                                 input_file, mace_out_file,
                                 input_names, input_shapes,
                                 input_data_formats,
                                 output_names, output_shapes,
                                 output_data_formats,
                                 validation_threshold,
                                 input_data_types,
                                 output_data_types, log_file)
    elif platform == 'keras':
        validate_keras_model(platform, device_type, model_file,
                             input_file, mace_out_file,
                             input_names, input_shapes,
                             input_data_formats,
                             output_names, output_shapes,
                             output_data_formats,
                             validation_threshold,
                             input_data_types,
                             output_data_types, log_file)


def parse_args():
    """Parses command line arguments."""
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--platform", type=str, default="", help="TensorFlow or Caffe.")
    parser.add_argument(
        "--model_file",
        type=str,
        default="",
        help="TensorFlow or Caffe \'GraphDef\' file to load.")
    parser.add_argument(
        "--weight_file",
        type=str,
        default="",
        help="caffe model file to load.")
    parser.add_argument(
        "--input_file", type=str, default="", help="input file.")
    parser.add_argument(
        "--mace_out_file",
        type=str,
        default="",
        help="mace output file to load.")
    parser.add_argument(
        "--device_type", type=str, default="", help="mace runtime device.")
    parser.add_argument(
        "--input_shape", type=str, default="1,64,64,3", help="input shape.")
    parser.add_argument(
        "--input_data_format", type=str, default="NHWC",
        help="input data format.")
    parser.add_argument(
        "--output_shape", type=str, default="1,64,64,2", help="output shape.")
    parser.add_argument(
        "--output_data_format", type=str, default="NHWC",
        help="output data format.")
    parser.add_argument(
        "--input_node", type=str, default="input_node", help="input node")
    parser.add_argument(
        "--input_data_type",
        type=str,
        default="",
        help="input data type")
    parser.add_argument(
        "--output_node", type=str, default="output_node", help="output node")
    parser.add_argument(
        "--validation_threshold", type=float, default=0.995,
        help="validation similarity threshold")
    parser.add_argument(
        "--backend",
        type=str,
        default="tensorflow",
        help="onnx backend framwork")
    parser.add_argument(
        "--validation_outputs_data", type=str,
        default="", help="validation outputs data file path.")
    parser.add_argument(
        "--log_file", type=str, default="", help="log file.")

    return parser.parse_known_args()


if __name__ == '__main__':
    FLAGS, unparsed = parse_args()
    validate(FLAGS.platform,
             FLAGS.model_file,
             FLAGS.weight_file,
             FLAGS.input_file,
             FLAGS.mace_out_file,
             FLAGS.device_type,
             FLAGS.input_shape,
             FLAGS.output_shape,
             FLAGS.input_data_format,
             FLAGS.output_data_format,
             FLAGS.input_node,
             FLAGS.output_node,
             FLAGS.validation_threshold,
             FLAGS.input_data_type,
             FLAGS.backend,
             FLAGS.validation_outputs_data,
             FLAGS.log_file)
