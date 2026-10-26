/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include <algorithm>

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ob_is_string_type(types_stack[CONTENT_IDX].get_type())
        ? types_stack[CONTENT_IDX].get_type() : ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator, *arg_content,
        expr.args_[CONTENT_IDX]->datum_meta_, expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->is_null() ? 0 : arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver) || OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                     const ObBitVector &skip, int64_t batch_size)
{
  int ret = OB_SUCCESS;
  struct InputRow {
    ObString model_;
    ObString content_;
    int64_t dimension_;
    int64_t row_;
  };
  struct OutputRow {
    ObString value_;
    int64_t row_;
  };
  // Keep requests within the supported native providers' small-batch limits.
  constexpr int64_t request_limit = 16;
  ObEvalCtx::TempAllocGuard alloc_guard(ctx);
  MultimodeAlloctor allocator(alloc_guard.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
  batch_guard.set_batch_size(batch_size);
  ObBitVector &evaluated = expr.get_evaluated_flags(ctx);
  ObBitVector &inactive = expr.get_pvt_skip(ctx);
  inactive.bit_calculate(skip, evaluated, batch_size, [](uint64_t left, uint64_t right) {
    return left | right;
  });
  ObArray<InputRow> inputs;
  ObArray<OutputRow> outputs;
  ObDatum *results = expr.locate_batch_datums(ctx);
  for (int64_t arg = 0; OB_SUCC(ret) && arg < expr.arg_cnt_; ++arg) {
    if (OB_FAIL(expr.args_[arg]->eval_batch(ctx, inactive, batch_size))) {
    }
  }
  ObDatumVector models = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
  ObDatumVector contents = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
  ObDatumVector dimensions;
  if (expr.arg_cnt_ == 3) {
    dimensions = expr.args_[DIM_IDX]->locate_expr_datumvector(ctx);
  }
  // Validate all active rows before issuing requests. NULL rows do not take up
  // request slots; explicit row numbers restore the original SQL ordering.
  for (int64_t row = 0; OB_SUCC(ret) && row < batch_size; ++row) {
    if (inactive.at(row)) {
      continue;
    } else if (models.at(row)->is_null() || contents.at(row)->is_null()) {
      results[row].set_null();
    } else {
      batch_guard.set_batch_idx(row);
      InputRow input{models.at(row)->get_string(), ObString(), 0, row};
      if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, *contents.at(row),
          expr.args_[CONTENT_IDX]->datum_meta_, expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), input.content_))) {
      } else if (input.model_.empty() || input.content_.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      } else if (expr.arg_cnt_ == 3 && (dimensions.at(row)->is_null() || dimensions.at(row)->get_int() <= 0)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
      } else {
        input.dimension_ = expr.arg_cnt_ == 3 ? dimensions.at(row)->get_int() : 0;
        if (OB_FAIL(inputs.push_back(input))) {
        }
      }
    }
  }
  if (OB_SUCC(ret) && inputs.count() > 1) {
    std::sort(inputs.begin(), inputs.end(), [](const InputRow &left, const InputRow &right) {
      const int model_order = left.model_.compare(right.model_);
      return model_order != 0 ? model_order < 0 :
          (left.dimension_ != right.dimension_ ? left.dimension_ < right.dimension_ : left.row_ < right.row_);
    });
  }
  query::ObIAiEndpointResolver *resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  for (int64_t group = 0; OB_SUCC(ret) && group < inputs.count();) {
    const InputRow &first = inputs.at(group);
    int64_t end = group + 1;
    while (end < inputs.count() && inputs.at(end).model_ == first.model_ &&
           inputs.at(end).dimension_ == first.dimension_) {
      ++end;
    }
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    int64_t inferred_dimension = 0;
    if (OB_ISNULL(resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, first.model_, info))) {
    } else if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(resolver->resolve_by_model_name(first.model_, allocator, endpoint))) {
    }
    for (int64_t base = group; OB_SUCC(ret) && base < end; base += request_limit) {
      const int64_t count = std::min(request_limit, end - base);
      ObArray<ObString> request;
      ObArray<ObString> response;
      ObJsonObject *config = nullptr;
      if (first.dimension_ > 0) {
        ObJsonInt *dimension = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, first.dimension_, dimension))) {
        } else if (OB_FAIL(config->add("dimensions", dimension))) {
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        if (OB_FAIL(request.push_back(inputs.at(base + i).content_))) {
        }
      }
      if (OB_SUCC(ret)) {
        ObAIFuncModel model(allocator, *info, endpoint);
        if (OB_FAIL(model.call_dense_embedding_vector_v2(request, config, response))) {
        } else if (response.count() != count) {
          ret = OB_INVALID_DATA;
        } else {
          if (first.dimension_ == 0) {
            ObIJsonBase *vector = nullptr;
            if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, response.at(0),
                ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, vector))) {
            } else if (OB_ISNULL(vector) || vector->json_type() != ObJsonNodeType::J_ARRAY ||
                       (inferred_dimension != 0 && inferred_dimension != vector->element_count())) {
              ret = OB_INVALID_DATA;
            } else {
              inferred_dimension = vector->element_count();
            }
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
            if (OB_FAIL(outputs.push_back(OutputRow{response.at(i), inputs.at(base + i).row_}))) {
            }
          }
        }
      }
    }
    group = end;
  }
  // Do not mark any row evaluated until every request and result copy succeeds.
  for (int64_t i = 0; OB_SUCC(ret) && i < outputs.count(); ++i) {
    OutputRow &output = outputs.at(i);
    batch_guard.set_batch_idx(output.row_);
    if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, results[output.row_], output.value_))) {
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t row = 0; row < batch_size; ++row) {
      if (!inactive.at(row)) {
        evaluated.set(row);
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
