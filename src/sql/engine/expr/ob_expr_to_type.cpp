/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_to_type.h"
#include "sql/engine/expr/ob_datum_cast.h"
#include "sql/session/ob_sql_session_info.h"
#include "common/sql_mode/ob_sql_mode_utils.h"

namespace oceanbase {
using namespace common;
namespace sql {

ObExprToType::ObExprToType(ObIAllocator& alloc)
    : ObFuncExprOperator::ObFuncExprOperator(alloc, T_FUN_SYS_TO_TYPE, N_TO_TYPE, 1, NOT_ROW_DIMENSION),
      expect_type_(ObMaxType),
      cast_mode_(CM_NONE)
{
  disable_operand_auto_cast();
}
int ObExprToType::calc_result_type1(ObExprResType& type, ObExprResType& type1, ObExprTypeCtx& type_ctx) const
{
  /*
   *
   * non-string===>string. use connection collation
   * non-string===>non-string. do not bother yourself to collation
   * string===>string. use collation of input param
   * string===>non-string. do not bother yourself to collation
   */
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ObMaxType == expect_type_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("expect type is not inited.", K(ret));
  } else if (OB_UNLIKELY(type1.is_null())) {
    type.set_null();
  } else if (type1.is_literal()) {
    ret = calc_result_type_for_literal(type, type1, type_ctx);
  } else {
    ret = calc_result_type_for_column(type, type1, type_ctx);
  }
  return ret;
}

int ObExprToType::calc_result1(ObObj& result, const ObObj& obj1, ObExprCtx& expr_ctx) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr_ctx.calc_buf_) || OB_ISNULL(expr_ctx.phy_plan_ctx_) || OB_ISNULL(expr_ctx.my_session_) ||
      ObMaxType == expect_type_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited",
        K(ret),
        K_(expr_ctx.calc_buf),
        K_(expr_ctx.phy_plan_ctx),
        K_(expr_ctx.my_session),
        K_(expect_type));
  } else {
    EXPR_DEFINE_CAST_CTX(expr_ctx, cast_mode_);
    if (ob_is_json(expect_type_)) {
      cast_ctx.dest_collation_ = result_type_.get_collation_type();
      bool is_bool = false;
      if (OB_FAIL(get_param_is_boolean(expr_ctx, obj1, is_bool))) {
        LOG_WARN("get is_boolean type failed, bool may be cast as json int", K(ret), K(obj1));
      } else {
        cast_ctx.cast_mode_ |= CM_TO_BOOLEAN;
      }
    }
    if (OB_FAIL(ObObjCaster::to_type(expect_type_, cast_ctx, obj1, result))) {
      LOG_WARN("failed to cast obj", K(ret));
    }
  }
  return ret;
}

int ObExprToType::assign(const ObExprOperator& other)
{
  int ret = OB_SUCCESS;
  if (other.get_type() != T_FUN_SYS_TO_TYPE) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("other operator type is unexpected", K(other.get_type()));
  } else {
    const ObExprToType& other_op = static_cast<const ObExprToType&>(other);
    expect_type_ = other_op.expect_type_;
    cast_mode_ = other_op.cast_mode_;
    if (OB_FAIL(ObFuncExprOperator::assign(other_op))) {
      LOG_WARN("assign other op failed", K(ret));
    }
  }
  return ret;
}

int ObExprToType::calc_result_type_for_literal(ObExprResType& type, ObExprResType& type1, ObExprTypeCtx& type_ctx) const
{
  /*
   * nonstring ===> string  cs_level = CS_LEVEL_COERCIBLE && cs_type = connection_collation
   * nonstring ===> nonstring cs_level = in.cs_level && cs_type = in.cs_type
   * string ===>string cs_level = in.cs_level && cs_type = in.cs_type
   * string ===>nonstring cs_levl = CS_LEVEL_NUMERIC cs_type = CS_TYPE_BINARY
   */
  int ret = OB_SUCCESS;
  if (OB_ISNULL(type_ctx.get_session())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument.", K(ret), K(type_ctx.get_session()));
  } else {
    ObArenaAllocator oballocator(ObModIds::OB_SQL_RES_TYPE);
    ObCastMode cast_mode = CM_WARN_ON_FAIL;
    const ObObj& in = type1.get_param();
    ObCollationType cast_coll_type = CS_TYPE_INVALID;
    bool nonstring_to_string = false;
    if ((!ob_is_string_type(type1.get_type())) && ob_is_string_or_lob_type(expect_type_)) {
      nonstring_to_string = true;
      if (CS_TYPE_INVALID == type_ctx.get_coll_type()) {
        LOG_ERROR("fail to get collation_connection, set it to default collation", K(ret));
        cast_coll_type = ObCharset::get_system_collation();
      } else {
        cast_coll_type = type_ctx.get_coll_type();
      }
    } else if (lib::is_mysql_mode() && ob_is_json(expect_type_)) {
      cast_coll_type = CS_TYPE_UTF8MB4_BIN;
      if (type1.has_result_flag(IS_BOOL_FLAG)) {
        cast_mode |= CM_TO_BOOLEAN;
      }
    }

    ObAccuracy res_accuracy;
    // so sad!  here we can not get the exact cur_time which will be used when cast time to datetime.
    // but it does not matter. since what we want is accuracy info rather than real result value
    // so we just pass 0 to the constructor of ObCastCtx.
    const ObDataTypeCastParams dtc_params = ObBasicSessionInfo::create_dtc_params(type_ctx.get_session());
    ObCastCtx cast_ctx(&oballocator, &dtc_params, cast_mode, cast_coll_type, &res_accuracy);
    ObObj out;
    if (OB_FAIL(ObObjCaster::to_type(expect_type_, cast_ctx, in, out))) {
      LOG_WARN("failed to cast obj", K(ret), K(in), K(expect_type_));
    } else {
      type.set_type(expect_type_);
      type.set_accuracy(res_accuracy);
      if (nonstring_to_string) {
        type.set_collation_level(CS_LEVEL_COERCIBLE);
        type.set_collation_type(out.get_collation_type());
      } else if (ob_is_string_or_lob_type(expect_type_) || ob_is_json(expect_type_)) {
        type.set_collation_level(out.get_collation_level());
        type.set_collation_type(out.get_collation_type());
      } else {
        type.set_collation_type(CS_TYPE_BINARY);
        type.set_collation_level(CS_LEVEL_NUMERIC);
      }
    }
  }
  return ret;
}

int ObExprToType::calc_result_type_for_column(ObExprResType& type, ObExprResType& type1, ObExprTypeCtx& type_ctx) const
{
  int ret = OB_SUCCESS;
  type.set_type(expect_type_);
  // deduce collation now.
  // get_compatibility_mode will not return OCEANBASE_MODE forever, so remove it.
  // if (OCEANBASE_MODE == get_compatibility_mode()) {
  //  ret = OB_INVALID_ARGUMENT;
  //  LOG_WARN("compatibility mode should not be oceanbase", K(ret));
  //} else {
  if ((!ob_is_string_or_lob_type(type1.get_type())) && ob_is_string_or_lob_type(expect_type_)) {
    ObCollationType collation_connection = type_ctx.get_coll_type();
    type.set_collation_level(CS_LEVEL_COERCIBLE);
    type.set_collation_type(collation_connection);
  } else if (ob_is_string_or_lob_type(expect_type_)) {
    type.set_collation_level(type1.get_collation_level());
    type.set_collation_type(type1.get_collation_type());
  } else if (lib::is_mysql_mode() && ob_is_json(expect_type_)) {
    type.set_collation_level(type1.get_collation_level());
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  } else {
    type.set_collation_type(CS_TYPE_BINARY);
    type.set_collation_level(CS_LEVEL_NUMERIC);
  }

  type.set_accuracy(ObAccuracy::MAX_ACCURACY2[get_compatibility_mode()][expect_type_]);

  if (ob_is_enumset_tc(type1.get_type())) {
    ObObjType calc_type = enumset_calc_types_[OBJ_TYPE_TO_CLASS[expect_type_]];
    if (OB_UNLIKELY(ObMaxType == calc_type)) {
      ret = OB_ERR_UNEXPECTED;
      SQL_ENG_LOG(WARN, "invalid type of parameter ", K(expect_type_), K(ret));
    } else if (ObVarcharType == calc_type) {
      type1.set_calc_type(calc_type);
    } else { /*do nothing*/
    }
  }
  //}
  return ret;
}

OB_SERIALIZE_MEMBER((ObExprToType, ObFuncExprOperator), expect_type_, cast_mode_);

int ObExprToType::cg_expr(ObExprCGCtx& expr_cg_ctx, const ObRawExpr& raw_expr, ObExpr& rt_expr) const
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  UNUSED(rt_expr);
  int ret = OB_ERR_UNEXPECTED;
  LOG_WARN("unexpected, new engine should not use to_type expr", K(ret));
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
