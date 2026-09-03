(function () {
    "use strict";

    /*
     * 项目行业词的唯一前端配置源。
     * 内容来自 backend/src/data/model/device_template.cpp 中 builtin_device_templates()
     * 注册的模板 ID、用户可见模板名称和字段显示名称。
     */
    window.EdgeKeyboardCommonWords = {
        deviceTypes: [
            { text: "RD100", pinyin: "" },
            { text: "接地电阻", pinyin: "jiedidianzu" },
            { text: "CM-RD100", pinyin: "" },
            { text: "通讯管理机-RD100", pinyin: "tongxunguanliji" },
            { text: "R2", pinyin: "" },
            { text: "R2测温", pinyin: "cewen" },
            { text: "R2A-0000H", pinyin: "" },
            { text: "R2A-1000H", pinyin: "" },
            { text: "R2A-2000H", pinyin: "" },
            { text: "R4-RD-1000H", pinyin: "" },
            { text: "R4接地电阻值", pinyin: "jiedidianzu" },
            { text: "R4-WT-1000H", pinyin: "" },
            { text: "R4无线测温值", pinyin: "wuxiancewen" },
            { text: "R4-RD-2000H", pinyin: "" },
            { text: "R4接地电阻详细", pinyin: "jiedidianzuxiangxi" },
            { text: "R4-WT-2000H", pinyin: "" },
            { text: "R4无线测温详细", pinyin: "wuxiancewenxiangxi" },
            { text: "CM-SF6", pinyin: "" },
            { text: "通讯管理机-SF6", pinyin: "tongxunguanliji" },
            { text: "CM-WT", pinyin: "" },
            { text: "通讯管理机-无线测温", pinyin: "tongxunguanlijiwuxiancewen" },
            { text: "EM100", pinyin: "" },
            { text: "绝缘监测", pinyin: "jueyuanjiance" }
        ],
        quantities: [
            { text: "接地电阻", pinyin: "jiedidianzu" },
            { text: "温度", pinyin: "wendu" },
            { text: "电池电压", pinyin: "dianchidianya" },
            { text: "信号强度", pinyin: "xinhaoqiangdu" },
            { text: "环境温度", pinyin: "huanjingwendu" },
            { text: "环境湿度", pinyin: "huanjingshidu" },
            { text: "氧气含量", pinyin: "yangqihanliang" },
            { text: "SF6浓度", pinyin: "nongdu" },
            { text: "上触头A温度", pinyin: "shangchutouwendu" },
            { text: "上触头B温度", pinyin: "shangchutouwendu" },
            { text: "上触头C温度", pinyin: "shangchutouwendu" },
            { text: "下触头A温度", pinyin: "xiachutouwendu" },
            { text: "下触头B温度", pinyin: "xiachutouwendu" },
            { text: "下触头C温度", pinyin: "xiachutouwendu" },
            { text: "绝缘电阻值", pinyin: "jueyuandianzuzhi" },
            { text: "剩余电流值", pinyin: "shengyudianliuzhi" },
            { text: "吸收比", pinyin: "xishoubi" },
            { text: "极化指标", pinyin: "jihuazhibiao" },
            { text: "当前状态", pinyin: "dangqianzhuangtai" },
            { text: "运行状态标志", pinyin: "yunxingzhuangtaibiaozhi" },
            { text: "绝缘电阻测量时间", pinyin: "jueyuandianzuceliangshijian" },
            { text: "放电时间", pinyin: "fangdianshijian" },
            { text: "绝缘电阻当前值", pinyin: "jueyuandianzudangqianzhi" },
            { text: "内部高压模块DC电压值", pinyin: "neibugaoyamokuaidianyazhi" },
            { text: "线路侧高压DC电压值", pinyin: "xianlucegaoyadianyazhi" },
            { text: "线路侧交流电压值", pinyin: "xianlucejiaoliudianyazhi" },
            { text: "线路侧交流电压频率", pinyin: "xianlucejiaoliudianyapinlv" }
        ]
    };
}());
